#include <mln/test/map_adapter.hpp>

#include <mln/map/map_options.hpp>
#include <mln/test/stub_file_source.hpp>
#include <mln/test/util.hpp>
#include <mln/util/image.hpp>
#include <mln/util/io.hpp>
#include <mln/util/run_loop.hpp>
#include <mln/style/layers/symbol_layer.hpp>
#include <mln/style/layers/circle_layer.hpp>
#include <mln/style/layers/line_layer.hpp>
#include <mln/style/style.hpp>
#include <mln/style/image.hpp>
#include <mln/style/source.hpp>
#include <mln/style/projection.hpp>
#include <mln/style/sources/geojson_source.hpp>
#include <mln/style/sources/vector_source.hpp>
#include <mln/style/conversion/filter.hpp>
#include <mln/style/conversion/json.hpp>
#include <mln/style/expression/dsl.hpp>
#include <mln/renderer/renderer.hpp>
#include <mln/gfx/headless_frontend.hpp>
#include <mln/actor/scheduler.hpp>

#include <mapbox/geometry/envelope.hpp>
#include <protozero/pbf_writer.hpp>

#include <array>

using namespace mln;
using namespace mln::style;
using namespace mln::style::expression;
using namespace std::literals;

namespace {

class QueryTest {
public:
    explicit QueryTest(const std::string& stylePath = "test/fixtures/api/query_style.json") {
        map.getStyle().loadJSON(util::read_file(stylePath));
        map.getStyle().addImage(std::make_unique<style::Image>(
            "test-icon", decodeImage(util::read_file("test/fixtures/sprites/default_marker.png")), 1.0f));

        frontend.render(map);
    }

    util::RunLoop loop;
    std::shared_ptr<StubFileSource> fileSource = std::make_shared<StubFileSource>();
    HeadlessFrontend frontend{1};
    MapAdapter map{frontend,
                   MapObserver::nullObserver(),
                   fileSource,
                   MapOptions().withMapMode(MapMode::Static).withSize(frontend.getSize())};
};

Filter parseFilter(const std::string& expression) {
    conversion::Error error;
    return *conversion::convertJSON<Filter>(expression, error);
}

std::vector<Feature> getTopClusterFeature(QueryTest& test) {
    test.fileSource->sourceResponse = [&](const Resource& resource) {
        EXPECT_EQ("http://url"s, resource.url);
        Response response;
        response.data = std::make_unique<std::string>(util::read_file("test/fixtures/supercluster/places.json"s));
        return response;
    };

    LatLng coordinate{0, 0};
    Mutable<GeoJSONOptions> options = makeMutable<GeoJSONOptions>();
    options->cluster = true;
    auto source = std::make_unique<GeoJSONSource>("cluster_source"s, std::move(options));
    source->setURL("http://url"s);
    source->loadDescription(*test.fileSource);

    auto clusterLayer = std::make_unique<SymbolLayer>("cluster_layer"s, "cluster_source"s);
    clusterLayer->setIconImage({"test-icon"s});
    clusterLayer->setIconSize(12.0f);

    test.map.jumpTo(CameraOptions().withCenter(coordinate).withZoom(0.0));
    test.map.getStyle().addSource(std::move(source));
    test.map.getStyle().addLayer(std::move(clusterLayer));
    test.loop.runOnce();
    test.frontend.render(test.map);

    auto screenCoordinate = test.map.pixelForLatLng(coordinate);
    const RenderedQueryOptions queryOptions({{{"cluster_layer"s}}, {}});
    return test.frontend.getRenderer()->queryRenderedFeatures(screenCoordinate, queryOptions);
}

} // end namespace

TEST(Query, QueryRenderedFeatures) {
    QueryTest test;

    // Batch conversion of latLngs to pixels
    auto points = test.map.pixelsForLatLngs({{0, 0}, {9, 9}});
    ASSERT_EQ(2, points.size());
    // Single conversion of latLng to pixel
    auto point0 = test.map.pixelForLatLng({0, 0});
    ASSERT_NEAR(points[0].x, point0.x, 1e-8);
    ASSERT_NEAR(points[0].y, point0.y, 1e-8);

    auto point1 = test.map.pixelForLatLng({9, 9});
    ASSERT_NEAR(points[1].x, point1.x, 1e-8);
    ASSERT_NEAR(points[1].y, point1.y, 1e-8);

    auto features1 = test.frontend.getRenderer()->queryRenderedFeatures(point0);
    EXPECT_EQ(features1.size(), 4u);

    auto features2 = test.frontend.getRenderer()->queryRenderedFeatures(point1);
    EXPECT_EQ(features2.size(), 0u);
}

TEST(Query, GlobeCircleCenterQueryExcludesTheFarSide) {
    QueryTest test("test/fixtures/api/globe_query_style.json");
    test.map.jumpTo(CameraOptions().withCenter(LatLng{0, 0}).withZoom(1.0));
    test.frontend.render(test.map);

    const auto point = test.map.pixelForLatLng({0, 0});
    const auto features = test.frontend.getRenderer()->queryRenderedFeatures(point, {{{"circles"}}, {}});

    ASSERT_EQ(features.size(), 1u);
    EXPECT_EQ(features.front().id, FeatureIdentifier{"center"s});
}

TEST(Query, GlobeCircleQueryFindsTheVisibleHorizonPoint) {
    QueryTest test("test/fixtures/api/globe_query_style.json");
    test.map.jumpTo(CameraOptions().withCenter(LatLng{0, 0}).withZoom(0.0));
    test.frontend.render(test.map);

    const auto point = test.map.pixelForLatLng({0, 75});
    const auto features = test.frontend.getRenderer()->queryRenderedFeatures(point, {{{"circles"}}, {}});

    ASSERT_EQ(features.size(), 1u);
    EXPECT_EQ(features.front().id, FeatureIdentifier{"horizon"s});
}

TEST(Query, GlobeCircleQueryUsesTheRenderedRadius) {
    QueryTest test("test/fixtures/api/globe_query_style.json");
    test.map.jumpTo(CameraOptions().withCenter(LatLng{0, 0}).withZoom(0.0));
    test.frontend.render(test.map);
    const RenderedQueryOptions options{{{"circles"}}, {}};

    const auto center = test.map.pixelForLatLng({0, 0});
    const auto inside = test.frontend.getRenderer()->queryRenderedFeatures(ScreenCoordinate{center.x + 3, center.y},
                                                                           options);
    ASSERT_EQ(inside.size(), 1u);
    EXPECT_EQ(inside.front().id, FeatureIdentifier{"center"s});
    EXPECT_TRUE(
        test.frontend.getRenderer()->queryRenderedFeatures(ScreenCoordinate{center.x + 9, center.y}, options).empty());
}

TEST(Query, GlobeCircleQueryFindsTheRenderedRadiusAtTheHorizon) {
    QueryTest test("test/fixtures/api/globe_query_style.json");
    test.map.jumpTo(CameraOptions().withCenter(LatLng{0, 0}).withZoom(0.0));
    test.frontend.render(test.map);
    const RenderedQueryOptions options{{{"circles"}}, {}};

    const auto center = test.map.pixelForLatLng({0, 75});
    for (const double offset : {-3.0, 3.0}) {
        const auto features = test.frontend.getRenderer()->queryRenderedFeatures(
            ScreenCoordinate{center.x + offset, center.y}, options);
        ASSERT_EQ(features.size(), 1u) << offset;
        EXPECT_EQ(features.front().id, FeatureIdentifier{"horizon"s});
    }
}

TEST(Query, GlobeCircleQueryFindsTheRenderedRadiusAtTheNorthernHorizon) {
    QueryTest test("test/fixtures/api/globe_query_style.json");
    test.map.jumpTo(CameraOptions().withCenter(LatLng{0, 0}).withZoom(0.0));
    test.frontend.render(test.map);
    const RenderedQueryOptions options{{{"circles"}}, {}};

    const auto center = test.map.pixelForLatLng({75, 0});
    for (const double offset : {-3.0, 3.0}) {
        const auto features = test.frontend.getRenderer()->queryRenderedFeatures(
            ScreenCoordinate{center.x, center.y + offset}, options);
        ASSERT_EQ(features.size(), 1u) << offset;
        EXPECT_EQ(features.front().id, FeatureIdentifier{"north"s});
    }
}

TEST(Query, GlobeCircleBoxQueryExcludesTheFarSide) {
    QueryTest test("test/fixtures/api/globe_query_style.json");
    test.map.jumpTo(CameraOptions().withCenter(LatLng{0, 0}).withZoom(0.0));
    test.frontend.render(test.map);

    const auto features = test.frontend.getRenderer()->queryRenderedFeatures(ScreenBox{{0, 0}, {256, 256}},
                                                                             {{{"circles"}}, {}});
    ASSERT_EQ(features.size(), 3u);
    for (const auto& feature : features) {
        EXPECT_TRUE(feature.id == FeatureIdentifier{"center"s} || feature.id == FeatureIdentifier{"horizon"s} ||
                    feature.id == FeatureIdentifier{"north"s});
    }
}

TEST(Query, GlobeCircleQueryUsesTheMapAlignedRadius) {
    QueryTest test("test/fixtures/api/globe_query_style.json");
    auto* layer = static_cast<CircleLayer*>(test.map.getStyle().getLayer("circles"));
    ASSERT_NE(layer, nullptr);
    layer->setCirclePitchAlignment(AlignmentType::Map);
    test.map.jumpTo(CameraOptions().withCenter(LatLng{0, 0}).withZoom(0.0));
    test.frontend.render(test.map);
    const RenderedQueryOptions options{{{"circles"}}, {}};

    const auto center = test.map.pixelForLatLng({0, 0});
    EXPECT_EQ(
        test.frontend.getRenderer()->queryRenderedFeatures(ScreenCoordinate{center.x + 3, center.y}, options).size(),
        1u);
    EXPECT_TRUE(
        test.frontend.getRenderer()->queryRenderedFeatures(ScreenCoordinate{center.x + 9, center.y}, options).empty());
}

TEST(Query, GlobeCircleQueryCrossesTheAntimeridian) {
    QueryTest test("test/fixtures/api/globe_query_style.json");
    test.map.jumpTo(CameraOptions().withCenter(LatLng{0, 170}).withZoom(1.0));
    test.frontend.render(test.map);

    const auto point = test.map.pixelForLatLng({0, -170});
    const auto features = test.frontend.getRenderer()->queryRenderedFeatures(point, {{{"circles"}}, {}});

    ASSERT_EQ(features.size(), 1u);
    EXPECT_EQ(features.front().id, FeatureIdentifier{"antimeridian"s});
}

TEST(Query, GlobeFillQueryDoesNotSnapSkyPointsToTheHorizon) {
    QueryTest test("test/fixtures/api/globe_query_style.json");
    test.map.jumpTo(CameraOptions().withCenter(LatLng{0, 0}).withZoom(0.0));
    test.frontend.render(test.map);
    const RenderedQueryOptions options{{{"fill"}}, {}};

    const auto center = test.map.pixelForLatLng({0, 0});
    const auto features = test.frontend.getRenderer()->queryRenderedFeatures(center, options);
    ASSERT_EQ(features.size(), 1u);
    EXPECT_EQ(features.front().id, FeatureIdentifier{"area"s});

    const auto horizon = test.map.pixelForLatLng({0, 75});
    EXPECT_FALSE(test.frontend.getRenderer()->queryRenderedFeatures(horizon, options).empty());
    EXPECT_TRUE(test.frontend.getRenderer()->queryRenderedFeatures(ScreenCoordinate{255, 128}, options).empty());
}

TEST(Query, GlobeFillBoxCanContainTheGlobeWithAllCornersInTheSky) {
    QueryTest test("test/fixtures/api/globe_query_style.json");
    test.map.jumpTo(CameraOptions().withCenter(LatLng{0, 0}).withZoom(0.0));
    test.frontend.render(test.map);

    const auto features = test.frontend.getRenderer()->queryRenderedFeatures(ScreenBox{{0, 0}, {256, 256}},
                                                                             {{{"fill"}}, {}});
    ASSERT_EQ(features.size(), 1u);
    EXPECT_EQ(features.front().id, FeatureIdentifier{"area"s});
}

TEST(Query, GlobeFillBoxDoesNotSnapSkyPointsToTheHorizon) {
    QueryTest test("test/fixtures/api/globe_query_style.json");
    test.map.jumpTo(CameraOptions().withCenter(LatLng{0, 0}).withZoom(0.0));
    test.frontend.render(test.map);

    const auto features = test.frontend.getRenderer()->queryRenderedFeatures(ScreenBox{{240, 120}, {255, 136}},
                                                                             {{{"fill"}}, {}});
    EXPECT_TRUE(features.empty());
}

TEST(Query, GlobeFillBoxIntersectsTheGlobeWhenItsCenterIsInTheSky) {
    QueryTest test("test/fixtures/api/globe_query_style.json");
    test.map.jumpTo(CameraOptions().withCenter(LatLng{0, 0}).withZoom(0.0));
    const auto rendered = test.frontend.render(test.map);
    const auto centerPixel = rendered.image.data.get() + 128 * rendered.image.stride() + 224 * 4;
    ASSERT_EQ(centerPixel[3], 0u);

    const auto features = test.frontend.getRenderer()->queryRenderedFeatures(ScreenBox{{192, 120}, {256, 136}},
                                                                             {{{"fill"}}, {}});
    ASSERT_EQ(features.size(), 1u);
    EXPECT_EQ(features.front().id, FeatureIdentifier{"area"s});
}

TEST(Query, GlobeLineBoxQueryCrossesTheAntimeridianDuringTransition) {
    QueryTest test("test/fixtures/api/globe_query_style.json");
    auto* source = static_cast<GeoJSONSource*>(test.map.getStyle().getSource("features"));
    ASSERT_NE(source, nullptr);
    GeoJSONFeature feature{LineString<double>{{{-179, -20}, {-179, 20}}}};
    feature.id = "antimeridian-route"s;
    source->setGeoJSON(GeoJSON{feature});
    test.map.jumpTo(CameraOptions().withCenter(LatLng{0, 179}).withZoom(1.0));

    for (const double transition : {0.2, 0.5, 0.98, 1.0}) {
        SCOPED_TRACE(transition);
        auto projection = std::make_unique<style::Projection>();
        projection->setType(ProjectionDefinition("mercator", "vertical-perspective", transition));
        test.map.getStyle().setProjection(std::move(projection));
        const auto rendered = test.frontend.render(test.map);
        unsigned renderedPixels = 0;
        for (uint32_t y = 120; y < 136; ++y) {
            for (uint32_t x = 112; x < 160; ++x) {
                const auto pixel = rendered.image.data.get() + y * rendered.image.stride() + x * 4;
                if (pixel[0] <= 5 && pixel[1] >= 250 && pixel[2] <= 5 && pixel[3] >= 250) {
                    ++renderedPixels;
                }
            }
        }
        ASSERT_GT(renderedPixels, 0u);

        const auto features = test.frontend.getRenderer()->queryRenderedFeatures(ScreenBox{{112, 120}, {160, 136}},
                                                                                 {{{"line"}}, {}});
        ASSERT_EQ(features.size(), 1u);
        EXPECT_EQ(features.front().id, FeatureIdentifier{"antimeridian-route"s});
    }
}

TEST(Query, GlobeLineBoxExcludesTheAntimeridianRouteOutsideItsRenderedArea) {
    QueryTest test("test/fixtures/api/globe_query_style.json");
    auto* source = static_cast<GeoJSONSource*>(test.map.getStyle().getSource("features"));
    ASSERT_NE(source, nullptr);
    GeoJSONFeature feature{LineString<double>{{{-179, -20}, {-179, 20}}}};
    feature.id = "antimeridian-route"s;
    source->setGeoJSON(GeoJSON{feature});
    test.map.jumpTo(CameraOptions().withCenter(LatLng{0, 179}).withZoom(1.0));

    for (const double transition : {0.2, 0.5, 0.98, 1.0}) {
        SCOPED_TRACE(transition);
        auto projection = std::make_unique<style::Projection>();
        projection->setType(ProjectionDefinition("mercator", "vertical-perspective", transition));
        test.map.getStyle().setProjection(std::move(projection));
        const auto rendered = test.frontend.render(test.map);
        for (uint32_t y = 120; y < 136; ++y) {
            for (uint32_t x = 176; x < 192; ++x) {
                const auto pixel = rendered.image.data.get() + y * rendered.image.stride() + x * 4;
                ASSERT_EQ(pixel[3], 0u);
            }
        }

        const auto features = test.frontend.getRenderer()->queryRenderedFeatures(ScreenBox{{176, 120}, {192, 136}},
                                                                                 {{{"line"}}, {}});
        EXPECT_TRUE(features.empty());
    }
}

TEST(Query, GlobeLineBoxDoesNotSnapSkyPointsToTheHorizon) {
    QueryTest test("test/fixtures/api/globe_query_style.json");
    test.map.jumpTo(CameraOptions().withCenter(LatLng{0, 0}).withZoom(0.0));
    test.frontend.render(test.map);

    const auto features = test.frontend.getRenderer()->queryRenderedFeatures(ScreenBox{{240, 120}, {255, 136}},
                                                                             {{{"line"}}, {}});
    EXPECT_TRUE(features.empty());
}

TEST(Query, MercatorLineBoxQueriesTheRenderedAntimeridianBuffer) {
    QueryTest test("test/fixtures/api/globe_query_style.json");
    auto projection = std::make_unique<style::Projection>();
    projection->setType(ProjectionDefinition("mercator"));
    test.map.getStyle().setProjection(std::move(projection));
    auto* source = static_cast<GeoJSONSource*>(test.map.getStyle().getSource("features"));
    ASSERT_NE(source, nullptr);
    GeoJSONFeature feature{LineString<double>{{{-179, -20}, {-179, 20}}}};
    feature.id = "antimeridian-route"s;
    source->setGeoJSON(GeoJSON{feature});
    test.map.jumpTo(CameraOptions().withCenter(LatLng{0, 179}).withZoom(1.0));
    const auto rendered = test.frontend.render(test.map);
    unsigned renderedPixels = 0;
    for (uint32_t y = 120; y < 136; ++y) {
        for (uint32_t x = 112; x < 160; ++x) {
            const auto pixel = rendered.image.data.get() + y * rendered.image.stride() + x * 4;
            if (pixel[0] <= 5 && pixel[1] >= 250 && pixel[2] <= 5 && pixel[3] >= 250) {
                ++renderedPixels;
            }
        }
        for (uint32_t x = 176; x < 192; ++x) {
            const auto pixel = rendered.image.data.get() + y * rendered.image.stride() + x * 4;
            ASSERT_EQ(pixel[3], 0u);
        }
    }
    ASSERT_GT(renderedPixels, 0u);

    const RenderedQueryOptions options{{{"line"}}, {}};
    const auto features = test.frontend.getRenderer()->queryRenderedFeatures(ScreenBox{{112, 120}, {160, 136}},
                                                                             options);
    ASSERT_EQ(features.size(), 1u);
    EXPECT_EQ(features.front().id, FeatureIdentifier{"antimeridian-route"s});
    EXPECT_TRUE(test.frontend.getRenderer()->queryRenderedFeatures(ScreenBox{{176, 120}, {192, 136}}, options).empty());
}

TEST(Query, GlobeLineQueryIgnoresAnEmptyVectorTileRing) {
    QueryTest test("test/fixtures/api/empty.json");
    std::string tileData;
    {
        protozero::pbf_writer tile{tileData};
        protozero::pbf_writer layer{tile, 3};
        layer.add_string(1, "routes");
        layer.add_uint32(5, 8192);
        layer.add_uint32(15, 2);
        {
            protozero::pbf_writer feature{layer, 2};
            feature.add_uint64(1, 1);
            feature.add_enum(3, 2);
            feature.add_bytes(4, "");
        }
        {
            protozero::pbf_writer feature{layer, 2};
            feature.add_uint64(1, 2);
            feature.add_enum(3, 2);
            const std::array<uint32_t, 6> route{9, 6144, 8192, 10, 4096, 0};
            feature.add_packed_uint32(4, route.begin(), route.end());
        }
    }
    test.fileSource->tileResponse = [data = std::make_shared<const std::string>(std::move(tileData))](const Resource&) {
        Response response;
        response.data = data;
        return response;
    };
    auto projection = std::make_unique<style::Projection>();
    projection->setType(ProjectionDefinition("vertical-perspective"));
    test.map.getStyle().setProjection(std::move(projection));
    test.map.getStyle().addSource(
        std::make_unique<VectorSource>("features", Tileset{{"https://example.com/{z}/{x}/{y}.mvt"}, {0, 0}}));
    auto line = std::make_unique<LineLayer>("line", "features");
    line->setSourceLayer("routes");
    line->setLineColor(Color{1, 0, 0, 1});
    line->setLineWidth(32);
    test.map.getStyle().addLayer(std::move(line));
    test.map.jumpTo(CameraOptions().withCenter(LatLng{0, 0}).withZoom(0.0));
    const auto rendered = test.frontend.render(test.map);
    const auto centerPixel = rendered.image.data.get() + 128 * rendered.image.stride() + 128 * 4;
    ASSERT_EQ(centerPixel[3], 255u);
    ASSERT_EQ(test.frontend.getRenderer()->querySourceFeatures("features", {{{"routes"}}, {}}).size(), 2u);

    const auto features = test.frontend.getRenderer()->queryRenderedFeatures(ScreenBox{{120, 120}, {136, 136}},
                                                                             {{{"line"}}, {}});
    ASSERT_EQ(features.size(), 1u);
    EXPECT_EQ(features.front().id, FeatureIdentifier{uint64_t{2}});
}

TEST(Query, GlobeLineBoxQueriesContainRenderedHorizonPixelsDuringTransition) {
    QueryTest test("test/fixtures/api/globe_query_style.json");
    test.map.jumpTo(CameraOptions().withCenter(LatLng{0, 0}).withZoom(0.0));
    const RenderedQueryOptions options{{{"line"}}, {}};

    for (const double transition : {0.2, 0.5, 0.98, 1.0}) {
        SCOPED_TRACE(transition);
        auto projection = std::make_unique<style::Projection>();
        projection->setType(ProjectionDefinition("mercator", "vertical-perspective", transition));
        test.map.getStyle().setProjection(std::move(projection));
        const auto rendered = test.frontend.render(test.map);
        unsigned renderedPixels = 0;

        for (uint32_t x = 0; x < rendered.image.size.width; ++x) {
            const auto pixel = rendered.image.data.get() + 128 * rendered.image.stride() + x * 4;
            if (pixel[0] > 5 || pixel[1] < 250 || pixel[2] > 5 || pixel[3] < 250) {
                continue;
            }
            ++renderedPixels;
            const auto features = test.frontend.getRenderer()->queryRenderedFeatures(
                ScreenBox{{double(x), 128}, {double(x + 1), 129}}, options);
            ASSERT_EQ(features.size(), 1u) << x;
            EXPECT_EQ(features.front().id, FeatureIdentifier{"route"s});
        }
        EXPECT_GT(renderedPixels, 0u);
    }
}

TEST(Query, GlobeFillBoxQueriesContainRenderedPixelsDuringTransition) {
    QueryTest test("test/fixtures/api/globe_query_style.json");
    test.map.jumpTo(CameraOptions().withCenter(LatLng{0, 0}).withZoom(0.0));
    const RenderedQueryOptions options{{{"fill"}}, {}};

    for (const double transition : {0.2, 0.5, 0.98, 1.0}) {
        SCOPED_TRACE(transition);
        auto projection = std::make_unique<style::Projection>();
        projection->setType(ProjectionDefinition("mercator", "vertical-perspective", transition));
        test.map.getStyle().setProjection(std::move(projection));
        const auto rendered = test.frontend.render(test.map);
        unsigned renderedPixels = 0;

        for (uint32_t x = 0; x < rendered.image.size.width; x += 8) {
            const auto pixel = rendered.image.data.get() + 128 * rendered.image.stride() + x * 4;
            if (pixel[0] != 51 || pixel[1] != 102 || pixel[2] != 153 || pixel[3] != 255) {
                continue;
            }
            ++renderedPixels;
            const auto features = test.frontend.getRenderer()->queryRenderedFeatures(
                ScreenBox{{double(x), 128}, {double(x + 1), 129}}, options);
            ASSERT_EQ(features.size(), 1u) << x;
            EXPECT_EQ(features.front().id, FeatureIdentifier{"area"s});
        }
        EXPECT_GT(renderedPixels, 0u);
    }
}

TEST(Query, GlobeLineBoxQueriesContainRenderedHighLatitudePixelsDuringTransition) {
    QueryTest test("test/fixtures/api/globe_query_style.json");
    test.map.jumpTo(CameraOptions().withCenter(LatLng{0, 0}).withZoom(0.0));
    const RenderedQueryOptions options{{{"line"}}, {}};

    for (const double transition : {0.2, 0.5, 0.98, 1.0}) {
        SCOPED_TRACE(transition);
        auto projection = std::make_unique<style::Projection>();
        projection->setType(ProjectionDefinition("mercator", "vertical-perspective", transition));
        test.map.getStyle().setProjection(std::move(projection));
        const auto rendered = test.frontend.render(test.map);
        unsigned renderedPixels = 0;

        for (uint32_t y = 0; y < rendered.image.size.height; ++y) {
            const auto pixel = rendered.image.data.get() + y * rendered.image.stride() + 128 * 4;
            if (pixel[0] > 5 || pixel[1] < 250 || pixel[2] > 5 || pixel[3] < 250) {
                continue;
            }
            ++renderedPixels;
            const auto features = test.frontend.getRenderer()->queryRenderedFeatures(
                ScreenBox{{128, double(y)}, {129, double(y + 1)}}, options);
            ASSERT_EQ(features.size(), 1u) << y;
            EXPECT_EQ(features.front().id, FeatureIdentifier{"north-route"s});
        }
        EXPECT_GT(renderedPixels, 0u);
    }
}

TEST(Query, GlobeBufferedCircleRemainsQueryableAcrossTileBoundary) {
    QueryTest test("test/fixtures/api/empty.json");
    auto projection = std::make_unique<style::Projection>();
    projection->setType(ProjectionDefinition("vertical-perspective"));
    test.map.getStyle().setProjection(std::move(projection));

    auto sourceOptions = makeMutable<GeoJSONOptions>();
    sourceOptions->maxzoom = 1;
    Immutable<GeoJSONOptions> options = std::move(sourceOptions);
    GeoJSONFeature feature{mln::Point<double>{-0.5, 1.0}};
    feature.id = "buffered"s;
    auto data = GeoJSONData::create(GeoJSON{feature}, Scheduler::GetSequenced(), options);
    std::optional<GeoJSONData::TileFeatures> bufferedTile;
    data->getTile({1, 1, 0}, [&](auto features) { bufferedTile = std::move(features); }, true);
    ASSERT_TRUE(bufferedTile);
    ASSERT_EQ(bufferedTile->size(), 1u);
    EXPECT_LT(mapbox::geometry::envelope(bufferedTile->front().geometry).max.x, 0);

    auto source = std::make_unique<GeoJSONSource>("buffered", std::move(options));
    source->setGeoJSONData(std::move(data));
    test.map.getStyle().addSource(std::move(source));
    auto circle = std::make_unique<CircleLayer>("buffered-circle", "buffered");
    circle->setCircleRadius(32.0f);
    circle->setCircleColor(Color{1, 0, 0, 1});
    test.map.getStyle().addLayer(std::move(circle));
    test.map.jumpTo(CameraOptions().withCenter(LatLng{1.0, 6.0}).withZoom(4.0));
    const auto rendered = test.frontend.render(test.map);
    EXPECT_LT(test.map.pixelForLatLng({1.0, -0.5}).x, 0);
    unsigned renderedPixels = 0;

    for (uint32_t x = 0; x < rendered.image.size.width; x += 4) {
        const auto pixel = rendered.image.data.get() + 128 * rendered.image.stride() + x * 4;
        if (pixel[0] < 250 || pixel[1] > 5 || pixel[2] > 5 || pixel[3] < 250) {
            continue;
        }
        ++renderedPixels;
        const auto features = test.frontend.getRenderer()->queryRenderedFeatures(
            ScreenCoordinate{double(x) + 0.5, 128.5}, {{{"buffered-circle"}}, {}});
        if (features.empty()) {
            ADD_FAILURE() << "Rendered buffered circle was not queryable at x=" << x;
            break;
        }
        EXPECT_EQ(features.front().id, FeatureIdentifier{"buffered"s});
    }
    EXPECT_GT(renderedPixels, 0u);
}

TEST(Query, QueryRenderedFeaturesFilterLayer) {
    QueryTest test;

    auto zz = test.map.pixelForLatLng({0, 0});

    auto features1 = test.frontend.getRenderer()->queryRenderedFeatures(zz, {{{"layer1"}}, {}});
    EXPECT_EQ(features1.size(), 1u);

    auto features2 = test.frontend.getRenderer()->queryRenderedFeatures(zz, {{{"layer1", "layer2"}}, {}});
    EXPECT_EQ(features2.size(), 2u);

    auto features3 = test.frontend.getRenderer()->queryRenderedFeatures(zz, {{{"foobar"}}, {}});
    EXPECT_EQ(features3.size(), 0u);

    auto features4 = test.frontend.getRenderer()->queryRenderedFeatures(zz, {{{"foobar", "layer3"}}, {}});
    EXPECT_EQ(features4.size(), 1u);
}

TEST(Query, QueryRenderedFeaturesFilter) {
    using namespace mln::style::expression::dsl;

    QueryTest test;
    auto zz = test.map.pixelForLatLng({0, 0});

    const Filter eqFilter(eq(get("key1"), literal("value1")));
    auto features1 = test.frontend.getRenderer()->queryRenderedFeatures(zz, {{}, {eqFilter}});
    EXPECT_EQ(features1.size(), 1u);

    const Filter idNotEqFilter(ne(id(), literal("feature1")));
    auto features2 = test.frontend.getRenderer()->queryRenderedFeatures(zz, {{{"layer4"}}, {idNotEqFilter}});
    EXPECT_EQ(features2.size(), 0u);

    const Filter gtFilter(gt(number(get("key2")), literal(1.0)));
    auto features3 = test.frontend.getRenderer()->queryRenderedFeatures(zz, {{}, {gtFilter}});
    EXPECT_EQ(features3.size(), 1u);
}

TEST(Query, GlobalStateChangeRelayoutsSourceFilter) {
    QueryTest test;
    Layer* layer = test.map.getStyle().getLayer("layer4");
    ASSERT_NE(layer, nullptr);
    layer->setFilter(parseFilter(R"(["==", ["get", "key1"], ["global-state", "filterValue"]])"));

    const auto point = test.map.pixelForLatLng({0, 0});
    const RenderedQueryOptions options{{{"layer4"}}, {}};

    test.map.getStyle().setGlobalStateProperty("filterValue", "value1");
    test.frontend.render(test.map);
    EXPECT_EQ(1u, test.frontend.getRenderer()->queryRenderedFeatures(point, options).size());

    test.map.getStyle().setGlobalStateProperty("filterValue", "other");
    test.frontend.render(test.map);
    EXPECT_TRUE(test.frontend.getRenderer()->queryRenderedFeatures(point, options).empty());

    test.map.getStyle().setGlobalStateProperty("filterValue", "value1");
    test.frontend.render(test.map);
    EXPECT_EQ(1u, test.frontend.getRenderer()->queryRenderedFeatures(point, options).size());
}

TEST(Query, QueryFiltersUseGlobalState) {
    QueryTest test;
    const auto filter = parseFilter(R"(["==", ["get", "key1"], ["global-state", "queryValue"]])");
    const auto point = test.map.pixelForLatLng({0, 0});
    const RenderedQueryOptions renderedOptions{{{"layer4"}}, {filter}};
    const SourceQueryOptions sourceOptions{{}, {filter}};

    test.map.getStyle().setGlobalStateProperty("queryValue", "value1");
    test.frontend.render(test.map);
    EXPECT_EQ(1u, test.frontend.getRenderer()->queryRenderedFeatures(point, renderedOptions).size());
    EXPECT_EQ(1u, test.frontend.getRenderer()->querySourceFeatures("source4", sourceOptions).size());

    test.map.getStyle().setGlobalStateProperty("queryValue", "other");
    test.frontend.render(test.map);
    EXPECT_TRUE(test.frontend.getRenderer()->queryRenderedFeatures(point, renderedOptions).empty());
    EXPECT_TRUE(test.frontend.getRenderer()->querySourceFeatures("source4", sourceOptions).empty());
}

TEST(Query, QuerySourceFeatures) {
    QueryTest test;

    auto features1 = test.frontend.getRenderer()->querySourceFeatures("source3");
    EXPECT_EQ(features1.size(), 1u);
}

TEST(Query, QuerySourceFeatureStates) {
    QueryTest test;

    FeatureState newState;
    newState["hover"] = true;
    newState["radius"].set<uint64_t>(20);
    test.frontend.getRenderer()->setFeatureState("source1", {}, "feature1", newState);

    FeatureState states;
    test.frontend.getRenderer()->getFeatureState(states, "source1", {}, "feature1");
    ASSERT_EQ(states.size(), 2u);
    ASSERT_EQ(states["hover"], true);
    ASSERT_EQ(states["radius"].get<uint64_t>(), 20u);
    ASSERT_EQ(newState, states);
}

TEST(Query, RemoveSourceFeatureState) {
    QueryTest test;
    auto* renderer = test.frontend.getRenderer();

    // Set two state values on a feature. Updates are visible immediately,
    // without waiting for a render pass.
    FeatureState newState;
    newState["hover"] = true;
    newState["radius"].set<uint64_t>(20);
    renderer->setFeatureState("source1", {}, "feature1", newState);

    // Read back with the out-parameter overload.
    FeatureState afterSet;
    renderer->getFeatureState(afterSet, "source1", {}, "feature1");
    ASSERT_EQ(afterSet, newState);

    // Remove a single key. Unlike updates, removals are only reflected once a
    // render pass has coalesced the pending changes into the source state, so
    // render before reading back.
    renderer->removeFeatureState("source1", {}, "feature1"s, "hover"s);
    test.frontend.render(test.map);

    // Read back with the return-value overload.
    const FeatureState afterKeyRemoval = renderer->getFeatureState("source1", {}, "feature1");
    ASSERT_EQ(afterKeyRemoval.size(), 1u);
    ASSERT_EQ(afterKeyRemoval.count("hover"), 0u);
    ASSERT_EQ(afterKeyRemoval.at("radius").get<uint64_t>(), 20u);

    // Removing the whole feature (no state key) clears any remaining state.
    renderer->removeFeatureState("source1", {}, "feature1"s, {});
    test.frontend.render(test.map);

    // Read back with the out-parameter overload again.
    FeatureState afterFeatureRemoval;
    renderer->getFeatureState(afterFeatureRemoval, "source1", {}, "feature1");
    ASSERT_TRUE(afterFeatureRemoval.empty());
}

TEST(Query, QuerySourceFeaturesOptionValidation) {
    QueryTest test;

    // GeoJSONSource, doesn't require a layer id
    auto features = test.frontend.getRenderer()->querySourceFeatures("source3");
    ASSERT_EQ(features.size(), 1u);

    // VectorSource, requires a layer id
    features = test.frontend.getRenderer()->querySourceFeatures("source5");
    ASSERT_EQ(features.size(), 0u);

    // RasterSource, not supported
    features = test.frontend.getRenderer()->querySourceFeatures("source6");
    ASSERT_EQ(features.size(), 0u);
}

TEST(Query, QuerySourceFeaturesFilter) {
    using namespace mln::style::expression::dsl;

    QueryTest test;

    const Filter eqFilter(eq(get("key1"), literal("value1")));
    auto features1 = test.frontend.getRenderer()->querySourceFeatures("source4", {{}, {eqFilter}});
    EXPECT_EQ(features1.size(), 1u);

    const Filter idNotEqFilter(ne(id(), literal("feature1")));
    auto features2 = test.frontend.getRenderer()->querySourceFeatures("source4", {{}, {idNotEqFilter}});
    EXPECT_EQ(features2.size(), 0u);

    const Filter gtFilter(gt(number(get("key2")), literal(1.0)));
    auto features3 = test.frontend.getRenderer()->querySourceFeatures("source4", {{}, {gtFilter}});
    EXPECT_EQ(features3.size(), 1u);
}

TEST(Query, QueryFeatureExtensionsInvalidExtension) {
    QueryTest test;

    auto unknownExt = test.frontend.getRenderer()->queryFeatureExtensions("source4"s, {}, "unknown"s, "children"s);
    auto unknownValue = unknownExt.get<mln::Value>();
    EXPECT_TRUE(unknownValue.is<NullValue>());
}

TEST(Query, QueryFeatureExtensionsSuperclusterChildren) {
    QueryTest test;
    auto topClusterFeature = getTopClusterFeature(test);

    ASSERT_EQ(topClusterFeature.size(), 1u);
    const auto featureProps = topClusterFeature[0].properties;
    auto clusterId = featureProps.find("cluster_id"s);
    auto cluster = featureProps.find("cluster"s);
    EXPECT_TRUE(clusterId != featureProps.end());
    EXPECT_TRUE(cluster != featureProps.end());

    auto queryChildren = test.frontend.getRenderer()->queryFeatureExtensions(
        "cluster_source"s, topClusterFeature[0], "supercluster"s, "children"s);

    EXPECT_TRUE(queryChildren.is<FeatureCollection>());
    auto children = queryChildren.get<FeatureCollection>();
    ASSERT_EQ(children.size(), 4u);

    // Compare results produced by supercluster with default clustering options.
    EXPECT_EQ(children[0].properties["cluster_id"].get<uint64_t>(), 2u);
    EXPECT_EQ(children[1].properties["cluster_id"].get<uint64_t>(), 34u);
    EXPECT_EQ(children[2].properties["cluster_id"].get<uint64_t>(), 258u);
    EXPECT_EQ(children[3].properties["cluster_id"].get<uint64_t>(), 2466u);
    EXPECT_EQ(children[0].properties["point_count"].get<uint64_t>(), 7u);
    EXPECT_EQ(children[1].properties["point_count"].get<uint64_t>(), 16u);
    EXPECT_EQ(children[2].properties["point_count"].get<uint64_t>(), 7u);
    EXPECT_EQ(children[3].properties["point_count"].get<uint64_t>(), 2u);
}

TEST(Query, QueryFeatureExtensionsSuperclusterExpansionZoom) {
    QueryTest test;
    auto topClusterFeature = getTopClusterFeature(test);
    ASSERT_EQ(topClusterFeature.size(), 1u);

    auto queryChildren = test.frontend.getRenderer()->queryFeatureExtensions(
        "cluster_source"s, topClusterFeature[0], "supercluster"s, "children"s);
    auto children = queryChildren.get<FeatureCollection>();

    auto queryExpansionZoom1 = test.frontend.getRenderer()->queryFeatureExtensions(
        "cluster_source"s, topClusterFeature[0], "supercluster"s, "expansion-zoom"s);

    auto queryExpansionZoom2 = test.frontend.getRenderer()->queryFeatureExtensions(
        "cluster_source"s, children[3], "supercluster"s, "expansion-zoom"s);
    auto zoomValue1 = queryExpansionZoom1.get<mln::Value>();
    auto zoomValue2 = queryExpansionZoom2.get<mln::Value>();
    EXPECT_TRUE(zoomValue1.is<uint64_t>());
    EXPECT_TRUE(zoomValue2.is<uint64_t>());
    EXPECT_EQ(zoomValue1.get<uint64_t>(), 1u);
    EXPECT_EQ(zoomValue2.get<uint64_t>(), 3u);
}

TEST(Query, QueryFeatureExtensionsSuperclusterLeaves) {
    QueryTest test;
    auto topClusterFeature = getTopClusterFeature(test);
    ASSERT_EQ(topClusterFeature.size(), 1u);

    // Get leaves for cluster 1, with default limit 10, offset 0.
    auto queryClusterLeaves = test.frontend.getRenderer()->queryFeatureExtensions(
        "cluster_source"s, topClusterFeature[0], "supercluster"s, "leaves"s);
    EXPECT_TRUE(queryClusterLeaves.is<FeatureCollection>());
    auto leaves = queryClusterLeaves.get<FeatureCollection>();
    EXPECT_EQ(leaves.size(), 10u);

    // Get leaves for cluster 1, with limit 3, offset 0.
    const std::map<std::string, mln::Value> limitOpts = {{"limit"s, static_cast<uint64_t>(3u)}};
    auto queryClusterLeavesLimit3 = test.frontend.getRenderer()->queryFeatureExtensions(
        "cluster_source"s, topClusterFeature[0], "supercluster"s, "leaves"s, limitOpts);
    auto limitLeaves3 = queryClusterLeavesLimit3.get<FeatureCollection>();
    ASSERT_EQ(limitLeaves3.size(), 3u);

    EXPECT_EQ(limitLeaves3[0].properties["name"].get<std::string>(), "Niagara Falls"s);
    EXPECT_EQ(limitLeaves3[1].properties["name"].get<std::string>(), "Cape May"s);
    EXPECT_EQ(limitLeaves3[2].properties["name"].get<std::string>(), "Cape Fear"s);

    // Get leaves for cluster 1, with limit 3, offset 3.
    const std::map<std::string, mln::Value> offsetOpts = {{"limit"s, static_cast<uint64_t>(3u)},
                                                          {"offset"s, static_cast<uint64_t>(3u)}};
    auto queryClusterLeavesOffset3 = test.frontend.getRenderer()->queryFeatureExtensions(
        "cluster_source"s, topClusterFeature[0], "supercluster"s, "leaves"s, offsetOpts);
    auto offsetLeaves3 = queryClusterLeavesOffset3.get<FeatureCollection>();
    EXPECT_EQ(offsetLeaves3.size(), 3u);
    EXPECT_EQ(offsetLeaves3[0].properties["name"].get<std::string>(), "Cape Hatteras"s);
    EXPECT_EQ(offsetLeaves3[1].properties["name"].get<std::string>(), "Cape Sable"s);
    EXPECT_EQ(offsetLeaves3[2].properties["name"].get<std::string>(), "Cape Cod"s);
}
