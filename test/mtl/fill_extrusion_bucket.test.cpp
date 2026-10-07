#if MLN_RENDER_BACKEND_METAL

#include <mln/test/util.hpp>
#include <mln/test/stub_geometry_tile_feature.hpp>
#include <mln/renderer/buckets/fill_extrusion_bucket.hpp>

#include <cmath>
#include <sstream>

using namespace mln;

namespace {

std::unique_ptr<FillExtrusionBucket> makeBucket(uint32_t granularity = 2, float rounded = 0) {
    auto bucket = std::make_unique<FillExtrusionBucket>(FillExtrusionBucket::PossiblyEvaluatedLayoutProperties{rounded},
                                                        std::map<std::string, Immutable<style::LayerProperties>>{},
                                                        16.0f,
                                                        1);
    bucket->setSubdivisionGranularity({.fill = {granularity, granularity}});
    bucket->setRetainFeaturesById(true);
    return bucket;
}

GeometryCollection square(int16_t low = 100, int16_t high = 1100) {
    return {{{low, low}, {high, low}, {high, high}, {low, high}, {low, low}}};
}

void append(FillExtrusionBucket& bucket,
            const GeometryCollection& geometry,
            uint64_t id = 1,
            const CanonicalTileID& canonical = {16, 1, 1}) {
    StubGeometryTileFeature feature{id, FeatureType::Polygon, geometry.clone(), {}};
    bucket.addFeature(feature, geometry, {}, {}, id, canonical);
}

void recordGeometry(const FillExtrusionBucket& bucket, const std::string& key) {
    const auto& vertices = bucket.vertices.vector();
    const auto& indices = bucket.triangles.vector();
    std::ostringstream roofs;
    std::ostringstream walls;
    std::ostringstream segments;
    std::size_t coveredVertices = 0;
    std::size_t coveredIndices = 0;
    for (const auto& segment : bucket.triangleSegments) {
        EXPECT_EQ(segment.vertexOffset, coveredVertices);
        EXPECT_EQ(segment.indexOffset, coveredIndices);
        EXPECT_LE(segment.vertexLength, 65535u);
        coveredVertices += segment.vertexLength;
        coveredIndices += segment.indexLength;
        segments << segment.vertexOffset << ',' << segment.vertexLength << ',' << segment.indexOffset << ','
                 << segment.indexLength << ';';
        for (std::size_t i = segment.indexOffset; i < segment.indexOffset + segment.indexLength; ++i) {
            ASSERT_LT(i, indices.size());
            ASSERT_LT(indices[i], segment.vertexLength);
            const auto index = segment.vertexOffset + indices[i];
            ASSERT_LT(index, vertices.size());
            const auto& vertex = vertices[index];
            roofs << vertex.a1[0] << ',' << vertex.a1[1] << ',' << (vertex.a2[0] >> 1) << ';';
        }
    }
    EXPECT_EQ(coveredVertices, vertices.size());
    EXPECT_EQ(coveredIndices, indices.size());
    std::size_t wallCount = 0;
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        if ((vertices[i].a2[0] & 1u) != 0) continue;
        ASSERT_LT(i + 1, vertices.size());
        ++wallCount;
        for (const auto index : {i, i + 1}) {
            const auto& vertex = vertices[index];
            walls << vertex.a1[0] << ',' << vertex.a1[1] << ',' << vertex.a2[0] << ',' << vertex.a2[1] << ';';
        }
    }
    ASSERT_EQ(bucket.instanceSegments.size(), 1u);
    EXPECT_EQ(bucket.instanceSegments.front().instanceCount, vertices.size());
    ::testing::Test::RecordProperty(key + "_roofs", roofs.str());
    ::testing::Test::RecordProperty(key + "_walls", walls.str());
    ::testing::Test::RecordProperty(key + "_segments", segments.str());
    ::testing::Test::RecordProperty(key + "_vertices", static_cast<int>(vertices.size()));
    ::testing::Test::RecordProperty(key + "_wall_count", static_cast<int>(wallCount));
}

} // namespace

TEST(MetalFillExtrusionBucket, SingleCellPreservesRoofCoordinatesAndWallEdges) {
    auto bucket = makeBucket();
    append(*bucket, square());
    recordGeometry(*bucket, "cell");
    EXPECT_EQ(bucket->vertices.elements(), 5u);
    EXPECT_EQ(bucket->instanceSegments.front().instanceCount, 5u);
    EXPECT_EQ(bucket->triangles.elements(), 6u);
    ASSERT_EQ(bucket->getRetainedFeatures().size(), 1u);
    EXPECT_EQ(bucket->getRetainedFeatures()[0].vertexCount, bucket->vertices.elements());
}

TEST(MetalFillExtrusionBucket, HoledGridPreservesInteriorRoofPointsAndArea) {
    auto bucket = makeBucket(8);
    auto geometry = square(0, 8192);
    geometry.push_back({{2048, 2048}, {2048, 6144}, {6144, 6144}, {6144, 2048}, {2048, 2048}});
    append(*bucket, geometry);
    recordGeometry(*bucket, "hole");
    const auto& vertices = bucket->vertices.vector();
    const auto& indices = bucket->triangles.vector();
    double twiceArea = 0;
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        const auto& a = vertices[indices[i]].a1;
        const auto& b = vertices[indices[i + 1]].a1;
        const auto& c = vertices[indices[i + 2]].a1;
        const double area = double(b[0] - a[0]) * (c[1] - a[1]) - double(b[1] - a[1]) * (c[0] - a[0]);
        EXPECT_LE(area, 0);
        twiceArea += area;
    }
    EXPECT_DOUBLE_EQ(std::abs(twiceArea) / 2, 50331648.0);
}

TEST(MetalFillExtrusionBucket, PoleAndWorldRoofsPreserveTriangleOrder) {
    for (const auto& canonical : {CanonicalTileID{1, 0, 0}, CanonicalTileID{1, 0, 1}, CanonicalTileID{0, 0, 0}}) {
        auto bucket = makeBucket();
        append(*bucket, square(-512, 8704), 1, canonical);
        recordGeometry(*bucket, "tile_" + std::to_string(canonical.z) + "_" + std::to_string(canonical.y));
        EXPECT_GT(bucket->triangles.elements(), 0u);
        bool north = false;
        bool south = false;
        for (const auto& vertex : bucket->vertices.vector()) {
            north |= vertex.a1[1] == -32768;
            south |= vertex.a1[1] == 32767;
        }
        EXPECT_EQ(north, canonical.y == 0);
        EXPECT_EQ(south, canonical.z == 0 || canonical.y == 1);
        if (canonical.z == 0) {
            for (const auto index : bucket->triangles.vector()) {
                EXPECT_GE(bucket->vertices.vector().at(index).a1[0], 0);
                EXPECT_LE(bucket->vertices.vector().at(index).a1[0], 8192);
            }
        }
    }
}

TEST(MetalFillExtrusionBucket, CoincidentFeaturesKeepSeparateRetainedVertexRanges) {
    auto bucket = makeBucket();
    append(*bucket, square(), 1);
    const auto firstCount = bucket->vertices.elements();
    append(*bucket, square(), 2);
    recordGeometry(*bucket, "features");
    EXPECT_EQ(firstCount, 5u);
    EXPECT_EQ(bucket->vertices.elements(), 10u);
    const auto& features = bucket->getRetainedFeatures();
    ASSERT_EQ(features.size(), 2u);
    EXPECT_NE(features[0].featureId, features[1].featureId);
    EXPECT_EQ(features[0].vertexOffset, 0u);
    EXPECT_EQ(features[0].vertexCount, firstCount);
    EXPECT_EQ(features[1].vertexOffset, firstCount);
    EXPECT_EQ(features[1].vertexCount, firstCount);
    const auto& indices = bucket->triangles.vector();
    ASSERT_EQ(indices.size(), 12u);
    for (std::size_t i = 6; i < indices.size(); ++i) EXPECT_GE(indices[i], firstCount);
}

TEST(MetalFillExtrusionBucket, SegmentRolloverKeepsLocalIndicesAtNonzeroVertexOffset) {
    auto bucket = makeBucket();
    for (uint64_t i = 0; i < 13098; ++i) append(*bucket, square(), i);
    ASSERT_EQ(bucket->triangleSegments.size(), 1u);
    EXPECT_EQ(bucket->triangleSegments.front().vertexLength, 65490u);
    append(*bucket, square(600, 1600), 13098);
    ASSERT_EQ(bucket->triangleSegments.size(), 1u);
    EXPECT_EQ(bucket->triangleSegments.front().vertexLength, 65495u);
    append(*bucket, square(800, 1800), 13099);
    ASSERT_EQ(bucket->triangleSegments.size(), 2u);
    const auto& segment = bucket->triangleSegments.back();
    EXPECT_EQ(segment.vertexOffset, 65495u);
    EXPECT_EQ(segment.vertexLength, 5u);
    EXPECT_GT(segment.vertexOffset, 0u);
    EXPECT_LE(segment.vertexLength, 65535u);
    const auto& indices = bucket->triangles.vector();
    const auto& vertices = bucket->vertices.vector();
    std::size_t coveredVertices = 0;
    std::size_t coveredIndices = 0;
    for (const auto& item : bucket->triangleSegments) {
        EXPECT_EQ(item.vertexOffset, coveredVertices);
        EXPECT_EQ(item.indexOffset, coveredIndices);
        EXPECT_LE(item.vertexLength, 65535u);
        coveredVertices += item.vertexLength;
        coveredIndices += item.indexLength;
        for (std::size_t i = item.indexOffset; i < coveredIndices; ++i) {
            ASSERT_LT(i, indices.size());
            EXPECT_LT(indices[i], item.vertexLength);
        }
    }
    EXPECT_EQ(coveredVertices, vertices.size());
    EXPECT_EQ(coveredIndices, indices.size());
    ASSERT_EQ(bucket->instanceSegments.size(), 1u);
    EXPECT_EQ(bucket->instanceSegments.front().instanceCount, vertices.size());
    for (std::size_t i = indices.size() - 6; i < indices.size(); ++i) {
        ASSERT_LT(indices[i], segment.vertexLength);
        const auto& point = vertices.at(segment.vertexOffset + indices[i]).a1;
        EXPECT_TRUE(point[0] == 800 || point[0] == 1800);
        EXPECT_TRUE(point[1] == 800 || point[1] == 1800);
    }
    RecordProperty("first_segment_vertices", static_cast<int>(bucket->triangleSegments.front().vertexLength));
    RecordProperty("last_segment_vertices", static_cast<int>(segment.vertexLength));
    RecordProperty("total_vertices", static_cast<int>(vertices.size()));
}

TEST(MetalFillExtrusionBucket, MultiplePolygonsWithinOneFeatureKeepTheirOwnRoofCoordinates) {
    auto bucket = makeBucket();
    auto geometry = square();
    auto second = square(2000, 3000);
    geometry.push_back(std::move(second[0]));
    append(*bucket, geometry);
    recordGeometry(*bucket, "polygons");
    EXPECT_EQ(bucket->vertices.elements(), 10u);
    ASSERT_EQ(bucket->getRetainedFeatures().size(), 1u);
    EXPECT_EQ(bucket->getRetainedFeatures()[0].vertexCount, bucket->vertices.elements());
    ASSERT_EQ(bucket->triangles.elements(), 12u);
    const auto& vertices = bucket->vertices.vector();
    const auto& indices = bucket->triangles.vector();
    for (std::size_t i = 0; i < indices.size(); ++i) {
        const auto& point = vertices.at(indices[i]).a1;
        const auto low = i < 6 ? 100 : 2000;
        const auto high = i < 6 ? 1100 : 3000;
        EXPECT_TRUE(point[0] == low || point[0] == high);
        EXPECT_TRUE(point[1] == low || point[1] == high);
    }
}

TEST(MetalFillExtrusionBucket, UnsubdividedRoofKeepsItsOriginalRingVertices) {
    auto bucket = makeBucket(1);
    append(*bucket, square());
    recordGeometry(*bucket, "unsubdivided");
    EXPECT_EQ(bucket->vertices.elements(), 5u);
    EXPECT_EQ(bucket->triangles.elements(), 6u);
}

TEST(MetalFillExtrusionBucket, RoundedRoofKeepsItsFractionalRingCoordinates) {
    auto bucket = makeBucket(2, 32);
    append(*bucket, square());
    recordGeometry(*bucket, "rounded");
    bool fractional = false;
    for (const auto& vertex : bucket->vertices.vector()) fractional |= (vertex.a2[0] >> 1) != 0;
    EXPECT_TRUE(fractional);
    EXPECT_GT(bucket->triangles.elements(), 6u);
}

TEST(MetalFillExtrusionBucket, OversizedRingThrowsBeforeWritingAnyVertices) {
    auto bucket = makeBucket();
    GeometryCoordinates ring;
    for (std::size_t i = 0; i < 65536; ++i) {
        const double angle = 6.283185307179586 * static_cast<double>(i) / 65536;
        ring.emplace_back(static_cast<int16_t>(16000 * std::cos(angle)), static_cast<int16_t>(16000 * std::sin(angle)));
    }
    ring.push_back(ring.front());
    EXPECT_ANY_THROW(append(*bucket, {ring}));
    EXPECT_EQ(bucket->vertices.elements(), 0u);
    EXPECT_EQ(bucket->triangles.elements(), 0u);
}

#endif
