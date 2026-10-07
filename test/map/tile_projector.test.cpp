#include <mln/test/util.hpp>

#include <mln/map/tile_projector.hpp>
#include <mln/map/transform.hpp>
#include <mln/map/vertical_perspective_projection.hpp>
#include <mln/style/projection.hpp>
#include <mln/util/projection.hpp>

#include <array>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#if MLN_RENDER_BACKEND_METAL
#include <mln/renderer/layer_tweaker.hpp>
#include <mln/shaders/layer_ubo.hpp>
#include <mln/shaders/mtl/common.hpp>

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#endif

using namespace mln;

namespace {

void setUpProjection(Transform& transform, const LatLng& center, double transition, double pitch = 0.0) {
    transform.resize({800, 600});
    transform.setProjectionDefinition(ProjectionDefinition("mercator", "vertical-perspective", transition));
    transform.jumpTo(CameraOptions().withCenter(center).withZoom(1.5).withBearing(37.0).withPitch(pitch).withPadding(
        EdgeInsets{35.0, 60.0, 10.0, 25.0}));
}

#if MLN_RENDER_BACKEND_METAL

struct ProjectionSample {
    ProjectionData data;
    mln::Point<double> point;
    vec3 sphere;
    double elevation = 0.0;
    bool projectTile = false;
};

std::vector<std::array<float, 4>> evaluateMetalProjection(MTL::Device& device,
                                                          const std::vector<ProjectionSample>& samples,
                                                          bool globe = true) {
    const std::string source = std::string(globe ? "#define PROJECTION_GLOBE\n" : "") + shaders::prelude + R"(
kernel void evaluateProjection(device const ProjectionUBO* projections [[buffer(0)]],
                               device const float4* points [[buffer(1)]],
                               device const float4* spheres [[buffer(2)]],
                               device float4* output [[buffer(3)]],
                               uint index [[thread_position_in_grid]]) {
    const float4 point = points[index];
#if defined(PROJECTION_GLOBE)
    output[index] = point.w > 0.5
        ? projectTile(point.xy, point.xy, projections[index])
        : interpolateProjection(point.xy, spheres[index].xyz, point.z, projections[index]);
#else
    output[index] = projectTile(point.xy, point.xy, projections[index]);
#endif
}
)";
    auto options = NS::TransferPtr(MTL::CompileOptions::alloc()->init());
    options->setFastMathEnabled(true);
    options->setLanguageVersion(MTL::LanguageVersion2_4);
    NS::Error* error = nullptr;
    auto library = NS::TransferPtr(
        device.newLibrary(NS::String::string(source.c_str(), NS::UTF8StringEncoding), options.get(), &error));
    if (!library) {
        ADD_FAILURE() << (error ? error->localizedDescription()->utf8String() : "Metal library is unavailable");
        return {};
    }
    auto function = NS::TransferPtr(
        library->newFunction(NS::String::string("evaluateProjection", NS::UTF8StringEncoding)));
    auto pipeline = NS::TransferPtr(device.newComputePipelineState(function.get(), &error));
    if (!pipeline) {
        ADD_FAILURE() << (error ? error->localizedDescription()->utf8String() : "Metal pipeline is unavailable");
        return {};
    }

    std::vector<shaders::ProjectionUBO> projections;
    std::vector<std::array<float, 4>> points;
    std::vector<std::array<float, 4>> spheres;
    for (const auto& sample : samples) {
        projections.push_back(LayerTweaker::toProjectionUBO(sample.data));
        points.push_back({static_cast<float>(sample.point.x),
                          static_cast<float>(sample.point.y),
                          static_cast<float>(sample.elevation),
                          sample.projectTile ? 1.0f : 0.0f});
        spheres.push_back({static_cast<float>(sample.sphere[0]),
                           static_cast<float>(sample.sphere[1]),
                           static_cast<float>(sample.sphere[2]),
                           0.0f});
    }
    auto projectionBuffer = NS::TransferPtr(device.newBuffer(
        projections.data(), projections.size() * sizeof(projections.front()), MTL::ResourceStorageModeShared));
    auto pointBuffer = NS::TransferPtr(
        device.newBuffer(points.data(), points.size() * sizeof(points.front()), MTL::ResourceStorageModeShared));
    auto sphereBuffer = NS::TransferPtr(
        device.newBuffer(spheres.data(), spheres.size() * sizeof(spheres.front()), MTL::ResourceStorageModeShared));
    auto outputBuffer = NS::TransferPtr(
        device.newBuffer(samples.size() * sizeof(std::array<float, 4>), MTL::ResourceStorageModeShared));
    auto queue = NS::TransferPtr(device.newCommandQueue());
    if (!projectionBuffer || !pointBuffer || !sphereBuffer || !outputBuffer || !queue) {
        ADD_FAILURE() << "Metal projection test could not allocate its buffers and queue";
        return {};
    }
    auto command = NS::RetainPtr(queue->commandBuffer());
    auto encoder = NS::RetainPtr(command->computeCommandEncoder());
    encoder->setComputePipelineState(pipeline.get());
    encoder->setBuffer(projectionBuffer.get(), 0, 0);
    encoder->setBuffer(pointBuffer.get(), 0, 1);
    encoder->setBuffer(sphereBuffer.get(), 0, 2);
    encoder->setBuffer(outputBuffer.get(), 0, 3);
    encoder->dispatchThreads(MTL::Size(samples.size(), 1, 1), MTL::Size(1, 1, 1));
    encoder->endEncoding();
    command->commit();
    command->waitUntilCompleted();
    if (command->status() != MTL::CommandBufferStatusCompleted) {
        ADD_FAILURE() << "Metal projection command did not complete";
        return {};
    }
    const auto* result = static_cast<const std::array<float, 4>*>(outputBuffer->contents());
    return {result, result + samples.size()};
}

#endif

} // namespace

TEST(GlobeProjection, BlendedCoordinatesRoundTripAcrossTheAntimeridian) {
    for (const double transition : {0.0,
                                    0.0001,
                                    0.2,
                                    0.5,
                                    0.98,
                                    0.999,
                                    static_cast<double>(std::nextafter(0.999f, 0.0f)),
                                    static_cast<double>(0.999f),
                                    static_cast<double>(std::nextafter(0.999f, 1.0f)),
                                    0.9999,
                                    1.0}) {
        for (const double pitch : {0.0, 40.0}) {
            Transform transform;
            setUpProjection(transform, {25.0, 175.0}, transition, pitch);
            for (const LatLng& coordinate : {LatLng{25.0, 175.0}, LatLng{35.0, 185.0}, LatLng{15.0, 165.0}}) {
                SCOPED_TRACE(testing::Message() << transition << ", " << pitch << ", " << coordinate.latitude() << ", "
                                                << coordinate.longitude());
                const ScreenCoordinate point = transform.latLngToScreenCoordinate(coordinate);
                const LatLng result = transform.screenCoordinateToLatLng(point, LatLng::Unwrapped);
                EXPECT_NEAR(coordinate.latitude(), result.latitude(), 1e-6);
                EXPECT_NEAR(coordinate.longitude(), result.longitude(), 1e-6);
            }
        }
    }
}

TEST(GlobeProjection, BlendedSkyHasNoSurfaceIntersection) {
    for (const double transition : {0.5, 0.98, 0.999, 1.0}) {
        Transform transform;
        setUpProjection(transform, {0.0, 0.0}, transition);
        EXPECT_FALSE(VerticalPerspectiveProjection::screenCoordinateToSurfaceIntersection(transform.getState(),
                                                                                          {-10000.0, -10000.0}));
    }
}

TEST(GlobeProjection, LatitudeAdjustedMinimumZoomKeepsTheCameraCenterVisible) {
    for (const double transition : {0.0001, 0.2, 0.5, 0.98, 1.0}) {
        for (const double latitude : {-80.0, -25.0, 0.0, 25.0, 80.0}) {
            Transform transform;
            transform.resize({800, 600});
            transform.setProjectionDefinition(ProjectionDefinition("mercator", "vertical-perspective", transition));
            transform.jumpTo(
                CameraOptions().withCenter(LatLng{latitude, 175.0}).withZoom(-4.0).withBearing(22.5).withPitch(30.0));
            const auto& state = transform.getState();
            const auto center = state.getLatLng(LatLng::Unwrapped);
            const auto point = state.latLngToScreenCoordinate(center);
            SCOPED_TRACE(testing::Message() << transition << ", latitude=" << latitude << ", zoom=" << state.getZoom()
                                            << ", point=" << point.x << ", " << point.y);
            EXPECT_NEAR(point.x, 400.0, 1e-6);
            EXPECT_NEAR(point.y, 300.0, 1e-6);
            EXPECT_FALSE(state.isLocationOccluded(center));
        }
    }
}

TEST(GlobeProjection, BlendedPoleInverseRoundTripsScreenPositions) {
    for (const double transition : {0.0001, 0.2}) {
        Transform transform;
        transform.resize({800, 600});
        transform.setProjectionDefinition(ProjectionDefinition("mercator", "vertical-perspective", transition));
        transform.jumpTo(
            CameraOptions().withCenter(LatLng{25.0, 175.0}).withZoom(0.0).withBearing(22.5).withPitch(30.0));
        for (const double latitude : {-90.0, 90.0}) {
            const LatLng coordinate{latitude, 245.0};
            const auto point = transform.latLngToScreenCoordinate(coordinate);
            const auto result = transform.screenCoordinateToLatLng(point, LatLng::Unwrapped);
            const auto roundTripPoint = transform.latLngToScreenCoordinate(result);
            SCOPED_TRACE(testing::Message() << transition << ", latitude=" << latitude);
            EXPECT_NEAR(roundTripPoint.x, point.x, 1e-4);
            EXPECT_NEAR(roundTripPoint.y, point.y, 1e-4);
        }
    }
}

#if MLN_RENDER_BACKEND_METAL

TEST(GlobeProjection, CameraCenterStaysAtViewportCenterDuringBidirectionalZoom) {
    auto pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
    auto device = NS::TransferPtr(MTL::CreateSystemDefaultDevice());
    if (!device) {
        GTEST_SKIP() << "Metal device is unavailable";
    }

    constexpr mln::Size viewport{390, 844};
    const LatLng center{-33.92, 151.22};
    constexpr int steps = 13 * 64;
    struct SampleState {
        double requestedZoom;
        double actualZoom;
        double latitude;
        double longitude;
        bool globe;
        bool reverse;
    };

    for (const auto projectionType : {ProjectionType::Mercator, ProjectionType::Globe}) {
        const std::string name = projectionType == ProjectionType::Globe ? "dynamic_globe" : "mercator";
        SCOPED_TRACE(name);
        style::Projection styleProjection;
        styleProjection.setType(ProjectionDefinition(projectionType));
        Transform transform;
        transform.resize(viewport);
        transform.setProjection(styleProjection.impl);
        transform.jumpTo(CameraOptions().withCenter(center).withZoom(3.0).withPitch(60.0).withBearing(33.0));

        std::vector<ProjectionSample> samples;
        std::vector<SampleState> states;
        std::array<std::vector<ProjectionSample>, 2> batches;
        std::array<std::vector<std::size_t>, 2> indexes;
        for (const bool reverse : {false, true}) {
            for (int step = 0; step <= steps; ++step) {
                const double requestedZoom = 3.0 + static_cast<double>(reverse ? steps - step : step) / 64.0;
                transform.jumpTo(CameraOptions().withZoom(requestedZoom));
                const auto& state = transform.getState();
                const auto actualCenter = state.getLatLng();
                ASSERT_NEAR(actualCenter.latitude(), center.latitude(), 1e-10);
                ASSERT_NEAR(actualCenter.longitude(), center.longitude(), 1e-10);
                ASSERT_NEAR(state.getZoom(), requestedZoom, 1e-10);

                const auto tileZoom = static_cast<uint8_t>(state.getIntegerZoom());
                const double tileCount = std::ldexp(1.0, tileZoom);
                const auto normalizedCenter = Projection::project(actualCenter, 1.0 / util::tileSize_D);
                const auto tileX = static_cast<uint32_t>(std::floor(normalizedCenter.x * tileCount));
                const auto tileY = static_cast<uint32_t>(std::floor(normalizedCenter.y * tileCount));
                const UnwrappedTileID tile{tileZoom, tileX, tileY};
                const mln::Point<double> point{(normalizedCenter.x * tileCount - tileX) * util::EXTENT,
                                               (normalizedCenter.y * tileCount - tileY) * util::EXTENT};
                const ProjectionSample sample{state.getProjectionData(tile), point, {}, 0.0, true};
                const bool globe = state.isGlobeRendering();
                const std::size_t batch = globe ? 1 : 0;
                indexes[batch].push_back(samples.size());
                batches[batch].push_back(sample);
                samples.push_back(sample);
                states.push_back({requestedZoom,
                                  state.getZoom(),
                                  actualCenter.latitude(),
                                  actualCenter.longitude(),
                                  globe,
                                  reverse});
            }
        }

        std::vector<std::array<float, 4>> results(samples.size());
        for (std::size_t batch = 0; batch < batches.size(); ++batch) {
            if (batches[batch].empty()) {
                continue;
            }
            const auto output = evaluateMetalProjection(*device.get(), batches[batch], batch == 1);
            ASSERT_EQ(output.size(), indexes[batch].size());
            for (std::size_t i = 0; i < output.size(); ++i) {
                results[indexes[batch][i]] = output[i];
            }
        }

        std::ostringstream rows;
        rows << std::setprecision(17)
             << "index,reverse,requested_zoom,actual_zoom,latitude,longitude,globe,transition,clip_x,clip_y,clip_z,"
                "clip_w,screen_x,screen_y,error_px\n";
        double maximumError = -1.0;
        double maximumStep = 0.0;
        std::size_t worstIndex = 0;
        mln::Point<double> previous;
        for (std::size_t i = 0; i < results.size(); ++i) {
            const auto& result = results[i];
            const auto& state = states[i];
            for (const auto value : result) {
                ASSERT_TRUE(std::isfinite(value));
            }
            ASSERT_GT(result[3], 0.0f);
            const mln::Point<double> screen{(0.5 + 0.5 * static_cast<double>(result[0]) / result[3]) * viewport.width,
                                            (0.5 - 0.5 * static_cast<double>(result[1]) / result[3]) * viewport.height};
            const double error = std::hypot(screen.x - viewport.width * 0.5, screen.y - viewport.height * 0.5);
            if (error > maximumError) {
                maximumError = error;
                worstIndex = i;
            }
            if (i > 0 && state.reverse == states[i - 1].reverse) {
                maximumStep = std::max(maximumStep, std::hypot(screen.x - previous.x, screen.y - previous.y));
            }
            previous = screen;
            rows << i << ',' << state.reverse << ',' << state.requestedZoom << ',' << state.actualZoom << ','
                 << state.latitude << ',' << state.longitude << ',' << state.globe << ','
                 << samples[i].data.projectionTransition << ',' << result[0] << ',' << result[1] << ',' << result[2]
                 << ',' << result[3] << ',' << screen.x << ',' << screen.y << ',' << error << '\n';
        }
        RecordProperty(name + "_maximum_error_px", std::to_string(maximumError));
        RecordProperty(name + "_maximum_step_px", std::to_string(maximumStep));
        std::ostringstream worst;
        worst << std::setprecision(17) << "index=" << worstIndex << ",zoom=" << states[worstIndex].actualZoom
              << ",transition=" << samples[worstIndex].data.projectionTransition << ",main_matrix=";
        for (const auto value : samples[worstIndex].data.mainMatrix) {
            worst << value << ',';
        }
        worst << "fallback_matrix=";
        for (const auto value : samples[worstIndex].data.fallbackMatrix) {
            worst << value << ',';
        }
        RecordProperty(name + "_worst_sample", worst.str());
        EXPECT_LE(maximumError, 0.25) << worst.str();
        EXPECT_LE(maximumStep, 0.25);
        if (HasFailure()) {
            RecordProperty(name + "_samples", rows.str());
        }
    }
}

TEST(GlobeProjection, SphereProjectionMatchesProductionMetalShader) {
    auto pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
    auto device = NS::TransferPtr(MTL::CreateSystemDefaultDevice());
    if (!device) {
        GTEST_SKIP() << "Metal device is unavailable";
    }
    Transform transform;
    setUpProjection(transform, {0.0, 0.0}, 1.0, 35.0);
    const auto base = transform.getState().getProjectionData(UnwrappedTileID{0, 0, 0});
    std::vector<ProjectionSample> samples;
    for (const double transition : {0.0, 0.0001, 0.2, 0.5, 0.98, 0.999, 0.9999, 1.0}) {
        for (const double depthOffset : {0.0, 0.25}) {
            auto data = base;
            data.projectionTransition = transition;
            data.depthOffset = depthOffset;
            for (const LatLng& coordinate :
                 {LatLng{0, 0}, LatLng{0, 80}, LatLng{0, 180}, LatLng{80, -40}, LatLng{-80, 40}}) {
                samples.push_back({data,
                                   Projection::project(coordinate, util::EXTENT / util::tileSize_D),
                                   VerticalPerspectiveProjection::surfaceVector(coordinate),
                                   1234.0});
            }
            samples.push_back({data, {4096.0, -32768.0}, {{0.0, 1.0, 0.0}}});
            samples.push_back({data, {4096.0, 32767.0}, {{0.0, -1.0, 0.0}}});
        }
    }
    const auto results = evaluateMetalProjection(*device.get(), samples);
    ASSERT_EQ(samples.size(), results.size());
    for (std::size_t index = 0; index < samples.size(); ++index) {
        const auto& sample = samples[index];
        SCOPED_TRACE(testing::Message() << index << ", " << sample.data.projectionTransition);
        const vec4 result = VerticalPerspectiveProjection::projectSphere(
            sample.data, sample.point, sample.sphere, sample.elevation);
        for (std::size_t component = 0; component < 4; ++component) {
            const double expected = results[index][component];
            EXPECT_NEAR(expected, result[component], std::max(0.0002, std::abs(expected) * 0.00003));
        }
    }
}

TEST(GlobeProjection, TileProjectionMatchesProductionMetalShader) {
    auto pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
    auto device = NS::TransferPtr(MTL::CreateSystemDefaultDevice());
    if (!device) {
        GTEST_SKIP() << "Metal device is unavailable";
    }
    Transform transform;
    setUpProjection(transform, {0.0, 0.0}, 1.0, 35.0);
    const TransformState& state = transform.getState();
    std::vector<ProjectionSample> samples;
    std::vector<UnwrappedTileID> tiles;
    for (const UnwrappedTileID& tile : {UnwrappedTileID{0, 0, 0}, UnwrappedTileID{2, 2, 1}}) {
        for (const double transition : {0.0, 0.2, 0.5, 0.98, 1.0}) {
            for (const vec2 translation : {vec2{{0.0, 0.0}}, vec2{{37.0, -23.0}}}) {
                auto data = state.getProjectionData(tile);
                data.projectionTransition = transition;
                data.depthOffset = 0.002;
                data.translate = translation;
                for (const mln::Point<double>& point : {mln::Point<double>{4096, 4096},
                                                        mln::Point<double>{200, 7900},
                                                        mln::Point<double>{4096, -32768},
                                                        mln::Point<double>{4096, 32767}}) {
                    samples.push_back({data, point, {}, 0.0, true});
                    tiles.push_back(tile);
                }
            }
        }
    }
    const auto results = evaluateMetalProjection(*device.get(), samples);
    ASSERT_EQ(samples.size(), results.size());
    for (std::size_t index = 0; index < samples.size(); ++index) {
        const auto& sample = samples[index];
        const auto& expected = results[index];
        SCOPED_TRACE(testing::Message() << index << ", " << sample.data.projectionTransition);
        const auto result = TileProjector(state, tiles[index], sample.data).project(sample.point);
        EXPECT_NEAR(expected[0] / expected[3], result.point.x, 0.00003);
        EXPECT_NEAR(expected[1] / expected[3], result.point.y, 0.00003);
        EXPECT_NEAR(expected[3], result.signedDistanceFromCamera, std::abs(expected[3]) * 0.00003);
        EXPECT_EQ(expected[3] <= 0.0 || expected[2] > expected[3] || expected[2] < 0.0, result.occluded);
    }
}

TEST(GlobeProjection, PublicCoordinatesAndOcclusionMatchProductionMetalShader) {
    auto pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
    auto device = NS::TransferPtr(MTL::CreateSystemDefaultDevice());
    if (!device) {
        GTEST_SKIP() << "Metal device is unavailable";
    }
    const std::array<LatLng, 5> coordinates{{{25.0, 175.0}, {35.0, 185.0}, {15.0, 165.0}, {-25.0, 355.0}, {0.0, 80.0}}};
    std::vector<ProjectionSample> samples;
    for (const double transition : {0.0001,
                                    0.2,
                                    0.5,
                                    0.98,
                                    0.999,
                                    static_cast<double>(std::nextafter(0.999f, 0.0f)),
                                    static_cast<double>(0.999f),
                                    static_cast<double>(std::nextafter(0.999f, 1.0f)),
                                    0.9999,
                                    1.0}) {
        Transform transform;
        setUpProjection(transform, {25.0, 175.0}, transition, 40.0);
        const auto data = transform.getState().getProjectionData(UnwrappedTileID{0, 0, 0});
        for (const auto& coordinate : coordinates) {
            samples.push_back({data,
                               Projection::project(coordinate, util::EXTENT / util::tileSize_D),
                               VerticalPerspectiveProjection::surfaceVector(coordinate)});
        }
    }
    const auto results = evaluateMetalProjection(*device.get(), samples);
    ASSERT_EQ(samples.size(), results.size());
    for (std::size_t index = 0; index < samples.size(); ++index) {
        const auto& sample = samples[index];
        const auto& expected = results[index];
        const LatLng& coordinate = coordinates[index % coordinates.size()];
        SCOPED_TRACE(testing::Message() << index << ", " << sample.data.projectionTransition);
        Transform transform;
        setUpProjection(transform, {25.0, 175.0}, sample.data.projectionTransition, 40.0);
        const ScreenCoordinate rendered{(expected[0] / expected[3] * 0.5 + 0.5) * 800.0,
                                        (0.5 - expected[1] / expected[3] * 0.5) * 600.0};
        const ScreenCoordinate result = transform.latLngToScreenCoordinate(coordinate);
        EXPECT_NEAR(rendered.x, result.x, 0.002);
        EXPECT_NEAR(rendered.y, result.y, 0.002);
        EXPECT_EQ(expected[3] <= 0.0 || expected[2] > expected[3] || expected[2] < 0.0,
                  transform.getState().isLocationOccluded(coordinate));
        if (index % coordinates.size() < 3) {
            const LatLng inverse = transform.screenCoordinateToLatLng(rendered, LatLng::Unwrapped);
            EXPECT_NEAR(coordinate.latitude(), inverse.latitude(), 0.001);
            EXPECT_NEAR(coordinate.longitude(), inverse.longitude(), 0.001);
        }
    }
}

#endif
