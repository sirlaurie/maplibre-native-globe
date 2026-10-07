#if MLN_RENDER_BACKEND_METAL

#include <mln/test/util.hpp>
#include <mln/test/map_adapter.hpp>
#include <mln/test/stub_file_source.hpp>

#include <mln/gfx/backend_scope.hpp>
#include <mln/gfx/context.hpp>
#include <mln/gfx/headless_frontend.hpp>
#include <mln/map/map_options.hpp>
#include <mln/style/style.hpp>
#include <mln/util/image.hpp>
#include <mln/util/run_loop.hpp>
#include <mln/util/timer.hpp>

using namespace mln;

TEST(HillshadeRenderTarget, VisibilityRoundTripRestoresThePreparationPass) {
    util::RunLoop loop;

    PremultipliedImage elevation({16, 16});
    for (uint32_t y = 0; y < 16; ++y) {
        for (uint32_t x = 0; x < 16; ++x) {
            auto* pixel = elevation.data.get() + (y * 16 + x) * 4;
            pixel[0] = 1;
            pixel[1] = static_cast<uint8_t>(x * 8 + y * 4);
            pixel[2] = 0;
            pixel[3] = 255;
        }
    }
    const auto tile = std::make_shared<const std::string>(encodePNG(elevation));
    auto fileSource = std::make_shared<StubFileSource>();
    fileSource->tileResponse = [tile](const Resource&) {
        Response response;
        response.data = tile;
        return response;
    };

    HeadlessFrontend frontend({64, 64}, 1);
    MapObserver observer;
    MapAdapter map(frontend, observer, fileSource, MapOptions().withMapMode(MapMode::Static).withSize({64, 64}));
    const auto render = [&] {
        HeadlessFrontend::RenderResult result;
        bool completed = false;
        std::exception_ptr error;
        gfx::BackendScope scope(*frontend.getBackend());
        util::Timer deadline;
        deadline.start(Seconds(30), Duration::zero(), [&] { loop.stop(); });
        map.renderStill([&](std::exception_ptr value) {
            error = value;
            completed = true;
            if (!error) {
                result.image = frontend.readStillImage();
                result.stats = frontend.getBackend()->getContext().renderingStats();
            }
            loop.stop();
        });
        if (!completed) {
            loop.run();
        }
        EXPECT_TRUE(completed);
        EXPECT_EQ(error, nullptr);
        return result;
    };
    map.getStyle().loadJSON(R"({
        "version":8,
        "sources":{"elevation":{"type":"raster-dem","tiles":["test://elevation/{z}/{x}/{y}"],"tileSize":16,"maxzoom":0}},
        "layers":[
            {"id":"hillshade","type":"hillshade","source":"elevation","minzoom":0.4}
        ]
    })");
    map.jumpTo(CameraOptions().withZoom(0.5));
    const auto warmup = render();
    ASSERT_TRUE(warmup.image.valid());
    const auto first = render();
    ASSERT_TRUE(first.image.valid());
    ASSERT_GT(first.stats.numDrawCalls, 0);

    map.jumpTo(CameraOptions().withZoom(0.25));
    const auto hidden = render();
    ASSERT_TRUE(hidden.image.valid());
    EXPECT_LT(hidden.stats.numDrawCalls, first.stats.numDrawCalls);

    map.jumpTo(CameraOptions().withZoom(0.5));
    const auto restored = render();
    ASSERT_TRUE(restored.image.valid());
    RecordProperty("initialRenderPasses", first.stats.numFrames - warmup.stats.numFrames);
    RecordProperty("restoredRenderPasses", restored.stats.numFrames - hidden.stats.numFrames);
    EXPECT_EQ(restored.stats.numFrames - hidden.stats.numFrames, first.stats.numFrames - warmup.stats.numFrames);
    EXPECT_EQ(restored.image, first.image);
}

#endif
