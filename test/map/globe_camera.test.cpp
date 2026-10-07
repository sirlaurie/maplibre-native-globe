#include <mln/test/map_adapter.hpp>
#include <mln/test/stub_file_source.hpp>
#include <mln/test/util.hpp>

#include <mln/gfx/headless_frontend.hpp>
#include <mln/map/map_options.hpp>
#include <mln/style/style.hpp>
#include <mln/util/run_loop.hpp>
#include <mln/util/unitbezier.hpp>

using namespace mln;

namespace {

class GlobeCameraMap : public MapAdapter {
public:
    using MapAdapter::MapAdapter;

    void advanceBy(Milliseconds elapsed) {
        impl->transform.updateTransitions(impl->transform.getTransitionStart() + elapsed);
    }
};

struct GlobeCameraTest {
    util::RunLoop loop;
    std::shared_ptr<StubFileSource> fileSource = std::make_shared<StubFileSource>();
    HeadlessFrontend frontend{Size{390, 874}, 1};
    GlobeCameraMap map{frontend,
                       MapObserver::nullObserver(),
                       fileSource,
                       MapOptions().withMapMode(MapMode::Static).withSize(frontend.getSize())};

    GlobeCameraTest() {
        map.getStyle().loadJSON(R"({"version":8,"projection":{"type":"globe"},"sources":{},"layers":[]})");
    }
};

CameraOptions streetCamera() {
    return CameraOptions().withCenter(LatLng{-33.8688, 151.2093}).withZoom(13.0);
}

CameraOptions globeCamera() {
    return CameraOptions().withCenter(LatLng{60.0, 10.0}).withZoom(-0.8);
}

void expectCamera(const GlobeCameraMap& map, const CameraOptions& expected) {
    const auto actual = map.getCameraOptions();
    ASSERT_TRUE(actual.center && actual.zoom);
    ASSERT_TRUE(expected.center && expected.zoom);
    EXPECT_NEAR(actual.center->latitude(), expected.center->latitude(), 1e-6);
    EXPECT_NEAR(actual.center->longitude(), expected.center->longitude(), 1e-6);
    EXPECT_NEAR(*actual.zoom, *expected.zoom, 1e-9);
}

void transition(GlobeCameraMap& map, const CameraOptions& target, bool fly, Milliseconds duration) {
    AnimationOptions animation{duration};
    animation.easing.emplace(0, 0, 1, 1);
    if (fly) {
        map.flyTo(target, animation);
    } else {
        map.easeTo(target, animation);
    }
}

void expectImmediateTarget(bool fly) {
    for (const bool reverse : {false, true}) {
        SCOPED_TRACE(reverse);
        GlobeCameraTest test;
        const auto start = reverse ? globeCamera() : streetCamera();
        const auto target = reverse ? streetCamera() : globeCamera();
        test.map.jumpTo(start);
        transition(test.map, target, fly, Milliseconds{0});
        expectCamera(test.map, target);
        EXPECT_EQ(test.map.getTransformState().isGlobeRendering(), !reverse);
    }
}

void expectStartFrame(bool fly) {
    for (const bool reverse : {false, true}) {
        SCOPED_TRACE(reverse);
        GlobeCameraTest test;
        const auto start = reverse ? globeCamera() : streetCamera();
        const auto target = reverse ? streetCamera() : globeCamera();
        test.map.jumpTo(start);
        transition(test.map, target, fly, Milliseconds{1000});
        test.map.advanceBy(Milliseconds{0});
        expectCamera(test.map, start);
        EXPECT_EQ(test.map.getTransformState().isGlobeRendering(), reverse);
    }
}

void expectSkippedFramesReachTarget(bool fly) {
    for (const bool reverse : {false, true}) {
        SCOPED_TRACE(reverse);
        GlobeCameraTest test;
        const auto start = reverse ? globeCamera() : streetCamera();
        const auto target = reverse ? streetCamera() : globeCamera();
        test.map.jumpTo(start);
        transition(test.map, target, fly, Milliseconds{1000});
        test.map.advanceBy(Milliseconds{0});
        test.map.advanceBy(Milliseconds{1000});
        expectCamera(test.map, target);
        EXPECT_EQ(test.map.getTransformState().isGlobeRendering(), !reverse);
    }
}

void expectProjectionAtEachFrame(bool fly) {
    for (const bool reverse : {false, true}) {
        SCOPED_TRACE(reverse);
        GlobeCameraTest test;
        const auto start = CameraOptions().withCenter(LatLng{60, 10}).withZoom(reverse ? -0.8 : 13.0);
        const auto target = CameraOptions().withCenter(LatLng{60, 10}).withZoom(reverse ? 13.0 : -0.8);
        test.map.jumpTo(start);
        transition(test.map, target, fly, Milliseconds{1000});
        for (const auto elapsed : {0, 50, 100, 150, 500, 850, 900, 950, 1000}) {
            SCOPED_TRACE(elapsed);
            test.map.advanceBy(Milliseconds{elapsed});
            const auto camera = test.map.getCameraOptions();
            ASSERT_TRUE(camera.zoom);
            const auto projection = test.map.getTransformState().getProjectionTransition();
            if (*camera.zoom <= 11) {
                EXPECT_DOUBLE_EQ(projection, 1);
            } else if (*camera.zoom >= 12) {
                EXPECT_DOUBLE_EQ(projection, 0);
            } else {
                EXPECT_NEAR(projection, 12 - *camera.zoom, 1e-6);
            }
        }
        expectCamera(test.map, target);
    }
}

} // namespace

TEST(Map, GlobeImmediateEaseToKeepsRequestedTarget) {
    expectImmediateTarget(false);
}

TEST(Map, GlobeImmediateFlyToKeepsRequestedTarget) {
    expectImmediateTarget(true);
}

TEST(Map, GlobeEaseToPreservesTheStartFrame) {
    expectStartFrame(false);
}

TEST(Map, GlobeFlyToPreservesTheStartFrame) {
    expectStartFrame(true);
}

TEST(Map, GlobeEaseToReachesTargetWhenIntermediateFramesAreSkipped) {
    expectSkippedFramesReachTarget(false);
}

TEST(Map, GlobeFlyToReachesTargetWhenIntermediateFramesAreSkipped) {
    expectSkippedFramesReachTarget(true);
}

TEST(Map, GlobeEaseToEvaluatesProjectionForEachFrameZoom) {
    expectProjectionAtEachFrame(false);
}

TEST(Map, GlobeFlyToEvaluatesProjectionForEachFrameZoom) {
    expectProjectionAtEachFrame(true);
}

TEST(Map, GlobeReplacedStyleDefinesTheNextCameraTransition) {
    GlobeCameraTest test;
    test.map.getStyle().loadJSON(R"({"version":8,"sources":{},"layers":[]})");
    test.map.jumpTo(streetCamera());
    auto style = std::make_unique<style::Style>(test.fileSource, 1, test.frontend.getThreadPool());
    style->loadJSON(R"({"version":8,"projection":{"type":"globe"},"sources":{},"layers":[]})");
    test.map.setStyle(std::move(style));

    transition(test.map, globeCamera(), false, Milliseconds{0});

    expectCamera(test.map, globeCamera());
    EXPECT_TRUE(test.map.getTransformState().isGlobeRendering());
}

TEST(Map, GlobeBoundsFitReachesStreetZoomWithoutLosingTheBounds) {
    GlobeCameraTest test;
    test.map.jumpTo(globeCamera());
    const auto bounds = LatLngBounds::hull({-33.869, 151.209}, {-33.868, 151.210});
    const EdgeInsets padding{30, 20, 60, 20};
    const auto camera = test.map.cameraForLatLngBounds(bounds, padding);
    ASSERT_TRUE(camera.zoom);
    EXPECT_GT(*camera.zoom, 12);

    test.map.jumpTo(camera);

    EXPECT_FALSE(test.map.getTransformState().isGlobeRendering());
    for (const auto& coordinate : {bounds.southwest(), bounds.southeast(), bounds.northwest(), bounds.northeast()}) {
        const auto point = test.map.pixelForLatLng(coordinate);
        EXPECT_GE(point.x, padding.left() - 1);
        EXPECT_LE(point.x, test.frontend.getSize().width - padding.right() + 1);
        EXPECT_GE(point.y, padding.top() - 1);
        EXPECT_LE(point.y, test.frontend.getSize().height - padding.bottom() + 1);
    }
}

TEST(Map, GlobeCameraBoundsMatchTheRequestedProjectionAcrossTheHandoff) {
    GlobeCameraTest test;
    for (const bool reverse : {false, true}) {
        SCOPED_TRACE(reverse);
        const auto start = reverse ? globeCamera() : streetCamera();
        const auto target = reverse ? streetCamera() : globeCamera();
        test.map.jumpTo(start);
        const auto bounds = test.map.latLngBoundsForCamera(target);

        test.map.jumpTo(target);

        const auto actual = test.map.latLngBoundsForCamera(test.map.getCameraOptions());
        EXPECT_NEAR(bounds.south(), actual.south(), 1e-9);
        EXPECT_NEAR(bounds.west(), actual.west(), 1e-9);
        EXPECT_NEAR(bounds.north(), actual.north(), 1e-9);
        EXPECT_NEAR(bounds.east(), actual.east(), 1e-9);
    }
}

TEST(Map, GlobeConstrainedPolarCenterHasAValidZoom) {
    for (const double latitude : {-89, 89}) {
        SCOPED_TRACE(latitude);
        GlobeCameraTest test;
        test.map.jumpTo(streetCamera());

        test.map.jumpTo(CameraOptions().withCenter(LatLng{latitude, 10}).withZoom(-5));

        EXPECT_TRUE(test.map.getTransformState().valid());
        const auto camera = test.map.getCameraOptions();
        ASSERT_TRUE(camera.center && camera.zoom);
        EXPECT_NEAR(std::abs(camera.center->latitude()), 85.0511287798, 1e-9);
        EXPECT_NEAR(*camera.zoom, -3.53505177974, 1e-9);
    }
}
