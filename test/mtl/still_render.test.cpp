#if MLN_RENDER_BACKEND_METAL

#include <mln/test/util.hpp>
#include <mln/test/map_adapter.hpp>
#include <mln/test/stub_file_source.hpp>

#include <mln/gfx/headless_frontend.hpp>
#include <mln/gfx/rendering_stats.hpp>
#include <mln/gfx/shader_registry.hpp>
#include <mln/mtl/context.hpp>
#include <mln/mtl/renderer_backend.hpp>
#include <mln/renderer/renderer_observer.hpp>
#include <mln/renderer/renderer.hpp>
#include <mln/shaders/mtl/fill.hpp>
#include <mln/shaders/program_parameters.hpp>
#include <mln/style/image.hpp>
#include <mln/style/style.hpp>
#include <mln/util/run_loop.hpp>
#include <mln/util/timer.hpp>

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <array>
#include <cstring>
#include <map>
#include <sstream>

using namespace mln;

namespace {

constexpr auto globeAreaStyle = R"({
    "version": 8,
    "projection": {"type": "globe"},
    "center": [0, 0],
    "zoom": 0,
    "sources": {
        "area": {
            "type": "geojson",
            "data": {
                "type": "Feature",
                "properties": {},
                "geometry": {
                    "type": "Polygon",
                    "coordinates": [[[-15, -15], [15, -15], [15, 15], [-15, 15], [-15, -15]]]
                }
            }
        }
    },
    "layers": [
        {"id": "background", "type": "background", "paint": {"background-color": "#0000ff"}},
        {"id": "area", "type": "fill", "source": "area", "paint": {"fill-color": "#ff0000", "fill-antialias": false}}
    ]
})";

class RecordingStillFrontend final : public HeadlessFrontend, public RendererObserver {
public:
    explicit RecordingStillFrontend(mln::Size size = {96, 96})
        : HeadlessFrontend(size, 1.0f) {}

    void setObserver(RendererObserver& observer) override {
        target = &observer;
        HeadlessFrontend::setObserver(*this);
    }

    void onInvalidate() override {
        ++invalidations;
        target->onInvalidate();
    }

    void onResourceError(std::exception_ptr error) override { target->onResourceError(error); }

    void onRenderError(std::exception_ptr error) override { target->onRenderError(error); }

    void onRegisterShaders(gfx::ShaderRegistry& registry) override { target->onRegisterShaders(registry); }

    void onPostCompileShader(shaders::BuiltIn shader, gfx::Backend::Type backend, const std::string& defines) override {
        shaderVariants[shader] |= defines.find("PROJECTION_GLOBE") == std::string::npos ? 1u : 2u;
        target->onPostCompileShader(shader, backend, defines);
    }

    void onShaderCompileFailed(shaders::BuiltIn shader,
                               gfx::Backend::Type backend,
                               const std::string& defines) override {
        target->onShaderCompileFailed(shader, backend, defines);
    }

    void onWillStartRenderingMap() override { target->onWillStartRenderingMap(); }

    void onWillStartRenderingFrame() override { target->onWillStartRenderingFrame(); }

    void onDidFinishRenderingFrame(RenderMode mode,
                                   bool repaint,
                                   bool placementChanged,
                                   std::shared_ptr<gfx::RenderingStats> stats) override {
        const auto& context = static_cast<mtl::Context&>(getBackend()->getContext());
        if (context.isRenderingDeferred()) {
            ++deferredFrames;
            EXPECT_EQ(mode, RenderMode::Partial);
        }
        if (mode == RenderMode::Full) {
            ++fullFrames;
            fullFrameStats = stats;
            EXPECT_FALSE(context.isRenderingDeferred());
            EXPECT_FALSE(context.hasRenderingFailed());
        }
        target->onDidFinishRenderingFrame(mode, repaint, placementChanged, std::move(stats));
    }

    void onDidFinishRenderingMap() override { target->onDidFinishRenderingMap(); }

    unsigned invalidations = 0;
    unsigned deferredFrames = 0;
    unsigned fullFrames = 0;
    std::shared_ptr<gfx::RenderingStats> fullFrameStats;
    std::map<shaders::BuiltIn, unsigned> shaderVariants;

private:
    RendererObserver* target = nullptr;
};

class FailedFillGroup final : public gfx::ShaderGroup {
public:
    gfx::ShaderPtr getOrCreateShader(gfx::Context& context,
                                     const StringIDSetsPair&,
                                     gfx::ProjectionVariant,
                                     std::string_view) override {
        if (!shader) {
            shader = static_cast<mtl::Context&>(context).createProgram(shaders::BuiltIn::FillShader,
                                                                       "FailedStillFill",
                                                                       "invalid metal source",
                                                                       "vertexMain",
                                                                       "fragmentMain",
                                                                       ProgramParameters(1.0f, false),
                                                                       {});
            using Source = shaders::ShaderSource<shaders::BuiltIn::FillShader, gfx::Backend::Type::Metal>;
            shader->initVertexAttribute(Source::attributes.front());
            ++creations;
        }
        return shader;
    }

    unsigned creations = 0;

private:
    std::shared_ptr<mtl::ShaderProgram> shader;
};

class FailedFillObserver final : public MapObserver {
public:
    void onRegisterShaders(gfx::ShaderRegistry& registry) override {
        EXPECT_TRUE(registry.replaceShader(std::shared_ptr<gfx::ShaderGroup>(group), "FillShader"));
    }

    void onShaderCompileFailed(shaders::BuiltIn shader, gfx::Backend::Type, const std::string&) override {
        if (shader == shaders::BuiltIn::FillShader) {
            ++failures;
        }
    }

    std::shared_ptr<FailedFillGroup> group = std::make_shared<FailedFillGroup>();
    unsigned failures = 0;
};

void addRedIcon(style::Style& style) {
    PremultipliedImage image({16, 16});
    for (std::size_t offset = 0; offset < image.bytes(); offset += 4) {
        image.data[offset] = 255;
        image.data[offset + 1] = 0;
        image.data[offset + 2] = 0;
        image.data[offset + 3] = 255;
    }
    style.addImage(std::make_unique<style::Image>("marker", std::move(image), 1.0f));
}

PremultipliedImage renderStillImage(MapAdapter& map, RecordingStillFrontend& frontend, util::RunLoop& loop) {
    const auto fullFramesBefore = frontend.fullFrames;
    bool completed = false;
    std::exception_ptr failure;
    PremultipliedImage image;
    map.renderStill([&](std::exception_ptr error) {
        completed = true;
        failure = error;
        if (!error) {
            image = frontend.readStillImage();
        }
        loop.stop();
    });
    util::Timer deadline;
    deadline.start(Seconds(30), mln::Duration::zero(), [&] { loop.stop(); });
    loop.run();
    EXPECT_TRUE(completed);
    EXPECT_EQ(failure, nullptr);
    EXPECT_GT(frontend.fullFrames, fullFramesBefore);
    return image;
}

void expectRedIconAt(const PremultipliedImage& image, uint32_t x, uint32_t y) {
    ASSERT_TRUE(image.valid());
    ASSERT_EQ(image.size, (mln::Size{96, 96}));
    uint32_t minX = image.size.width;
    uint32_t minY = image.size.height;
    uint32_t maxX = 0;
    uint32_t maxY = 0;
    std::size_t redPixels = 0;
    for (uint32_t pixelY = 0; pixelY < image.size.height; ++pixelY) {
        for (uint32_t pixelX = 0; pixelX < image.size.width; ++pixelX) {
            const auto* pixel = image.data.get() + 4 * (pixelY * image.size.width + pixelX);
            if (pixel[0] > 128 && pixel[2] < 128) {
                minX = std::min(minX, pixelX);
                minY = std::min(minY, pixelY);
                maxX = std::max(maxX, pixelX);
                maxY = std::max(maxY, pixelY);
                ++redPixels;
            }
        }
    }
    SCOPED_TRACE(::testing::Message() << "red pixels " << redPixels << " bounds " << minX << "," << minY << "-" << maxX
                                      << "," << maxY);
    for (const uint32_t pixelY : {y - 3, y, y + 3}) {
        for (const uint32_t pixelX : {x - 3, x, x + 3}) {
            SCOPED_TRACE(::testing::Message() << "pixel " << pixelX << "," << pixelY);
            const auto* pixel = image.data.get() + 4 * (pixelY * image.size.width + pixelX);
            EXPECT_EQ(pixel[0], 255u);
            EXPECT_EQ(pixel[1], 0u);
            EXPECT_EQ(pixel[2], 0u);
            EXPECT_EQ(pixel[3], 255u);
        }
    }
    for (const uint32_t pixelX : {x - 12, x + 12}) {
        const auto* pixel = image.data.get() + 4 * (y * image.size.width + pixelX);
        EXPECT_EQ(pixel[0], 0u);
        EXPECT_EQ(pixel[1], 0u);
        EXPECT_EQ(pixel[2], 255u);
        EXPECT_EQ(pixel[3], 255u);
    }
}

} // namespace

TEST(MetalStillRender, GlobeStillCompletesAfterPendingShadersProduceTheFillAndBackground) {
    auto pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
    auto device = NS::TransferPtr(MTL::CreateSystemDefaultDevice());
    if (!device) {
        GTEST_SKIP() << "Metal device is unavailable";
    }
    util::RunLoop loop;
    RecordingStillFrontend frontend;
    auto fileSource = std::make_shared<StubFileSource>(StubFileSource::ResponseType::Synchronous);
    MapAdapter map(frontend,
                   MapObserver::nullObserver(),
                   fileSource,
                   MapOptions().withMapMode(MapMode::Static).withSize(mln::Size{96, 96}));
    map.getStyle().loadJSON(globeAreaStyle);

    unsigned completions = 0;
    std::exception_ptr failure;
    PremultipliedImage image;
    map.renderStill([&](std::exception_ptr error) {
        ++completions;
        failure = error;
        if (!error) {
            image = frontend.readStillImage();
        }
        loop.stop();
    });
    util::Timer deadline;
    deadline.start(Seconds(30), mln::Duration::zero(), [&] { loop.stop(); });
    loop.run();

    ASSERT_EQ(completions, 1u);
    ASSERT_EQ(failure, nullptr);
    EXPECT_GT(frontend.invalidations, 0u);
    EXPECT_GT(frontend.deferredFrames, 0u);
    EXPECT_GT(frontend.fullFrames, 0u);
    ASSERT_TRUE(image.valid());
    ASSERT_EQ(image.size, (mln::Size{96, 96}));
    const auto center = image.data.get() + 4 * (48 * 96 + 48);
    EXPECT_EQ(center[0], 255u);
    EXPECT_EQ(center[1], 0u);
    EXPECT_EQ(center[2], 0u);
    EXPECT_EQ(center[3], 255u);
    std::size_t bluePixels = 0;
    for (std::size_t offset = 0; offset < image.bytes(); offset += 4) {
        if (image.data[offset] == 0 && image.data[offset + 1] == 0 && image.data[offset + 2] == 255 &&
            image.data[offset + 3] == 255) {
            ++bluePixels;
        }
    }
    EXPECT_GT(bluePixels, 16u);
}

TEST(MetalStillRender, ASecondStillRequestReceivesTheCachedShaderError) {
    auto pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
    auto device = NS::TransferPtr(MTL::CreateSystemDefaultDevice());
    if (!device) {
        GTEST_SKIP() << "Metal device is unavailable";
    }
    util::RunLoop loop;
    RecordingStillFrontend frontend;
    auto fileSource = std::make_shared<StubFileSource>(StubFileSource::ResponseType::Synchronous);
    FailedFillObserver observer;
    MapAdapter map(
        frontend, observer, fileSource, MapOptions().withMapMode(MapMode::Static).withSize(mln::Size{96, 96}));
    map.getStyle().loadJSON(globeAreaStyle);
    std::vector<std::exception_ptr> results;
    const auto requestStill = [&] {
        bool completed = false;
        map.renderStill([&](std::exception_ptr error) {
            completed = true;
            results.push_back(error);
            loop.stop();
        });
        util::Timer deadline;
        deadline.start(Seconds(30), mln::Duration::zero(), [&] { loop.stop(); });
        loop.run();
        return completed;
    };

    ASSERT_TRUE(requestStill());
    ASSERT_EQ(results.size(), 1u);
    ASSERT_NE(results.front(), nullptr);
    ASSERT_TRUE(requestStill());
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0], results[1]);
    EXPECT_EQ(observer.group->creations, 1u);
    EXPECT_EQ(observer.failures, 1u);
    EXPECT_EQ(frontend.fullFrames, 0u);
}

TEST(MetalStillRender, ViewportIconKeepsItsPixelOffsetAcrossTheGlobeMercatorHandoff) {
    auto pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
    auto device = NS::TransferPtr(MTL::CreateSystemDefaultDevice());
    if (!device) {
        GTEST_SKIP() << "Metal device is unavailable";
    }
    util::RunLoop loop;
    RecordingStillFrontend frontend;
    auto fileSource = std::make_shared<StubFileSource>(StubFileSource::ResponseType::Synchronous);
    MapAdapter map(frontend,
                   MapObserver::nullObserver(),
                   fileSource,
                   MapOptions().withMapMode(MapMode::Static).withSize(mln::Size{96, 96}));
    map.getStyle().loadJSON(R"({
        "version": 8,
        "projection": {"type": "globe"},
        "center": [0, 0],
        "zoom": 11.5,
        "pitch": 45,
        "bearing": 33,
        "sources": {
            "point": {
                "type": "geojson",
                "maxzoom": 0,
                "data": {"type": "Point", "coordinates": [0, 0]}
            }
        },
        "layers": [
            {"id": "background", "type": "background", "paint": {"background-color": "#0000ff"}},
            {
                "id": "marker",
                "type": "symbol",
                "source": "point",
                "layout": {
                    "icon-image": "marker",
                    "icon-offset": [16, -12],
                    "icon-pitch-alignment": "viewport",
                    "icon-rotation-alignment": "viewport",
                    "icon-allow-overlap": true,
                    "icon-ignore-placement": true
                }
            }
        ]
    })");
    addRedIcon(map.getStyle());

    for (const double zoom : {11.5, 12.0, 11.75, 16.0}) {
        SCOPED_TRACE(::testing::Message() << "zoom " << zoom);
        map.jumpTo(CameraOptions().withZoom(zoom));
        const auto image = renderStillImage(map, frontend, loop);
        expectRedIconAt(image, 64, 36);
    }
}

TEST(MetalStillRender, MercatorScreenSpaceIconKeepsItsWorldTilePixelPositionAtStreetZooms) {
    auto pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
    auto device = NS::TransferPtr(MTL::CreateSystemDefaultDevice());
    if (!device) {
        GTEST_SKIP() << "Metal device is unavailable";
    }
    util::RunLoop loop;
    RecordingStillFrontend frontend;
    auto fileSource = std::make_shared<StubFileSource>(StubFileSource::ResponseType::Synchronous);
    MapAdapter map(frontend,
                   MapObserver::nullObserver(),
                   fileSource,
                   MapOptions().withMapMode(MapMode::Static).withSize(mln::Size{96, 96}));
    map.getStyle().loadJSON(R"({
        "version": 8,
        "projection": {"type": "globe"},
        "center": [45, 0],
        "zoom": 16,
        "pitch": 60,
        "bearing": 37,
        "sources": {
            "point": {
                "type": "geojson",
                "maxzoom": 0,
                "data": {"type": "Point", "coordinates": [45, 0]}
            }
        },
        "layers": [
            {"id": "background", "type": "background", "paint": {"background-color": "#0000ff"}},
            {
                "id": "marker",
                "type": "symbol",
                "source": "point",
                "layout": {
                    "symbol-screen-space": true,
                    "icon-image": "marker",
                    "icon-offset": [0, 0],
                    "icon-pitch-alignment": "viewport",
                    "icon-rotation-alignment": "viewport",
                    "icon-allow-overlap": true,
                    "icon-ignore-placement": true
                }
            }
        ]
    })");
    addRedIcon(map.getStyle());

    for (const double zoom : {16.0, 18.0}) {
        SCOPED_TRACE(::testing::Message() << "zoom " << zoom);
        map.jumpTo(CameraOptions().withZoom(zoom));
        const auto image = renderStillImage(map, frontend, loop);
        expectRedIconAt(image, 60, 48);
    }
}

TEST(MetalStillRender, ExtrusionsKeepTheirPixelsAcrossProjectionRoundTrips) {
    auto pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
    auto device = NS::TransferPtr(MTL::CreateSystemDefaultDevice());
    if (!device) {
        GTEST_SKIP() << "Metal device is unavailable";
    }
    util::RunLoop loop;
    RecordingStillFrontend frontend({512, 512});
    auto fileSource = std::make_shared<StubFileSource>(StubFileSource::ResponseType::Synchronous);
    MapAdapter map(frontend,
                   MapObserver::nullObserver(),
                   fileSource,
                   MapOptions().withMapMode(MapMode::Static).withSize(mln::Size{512, 512}));

    for (const bool patterned : {false, true}) {
        SCOPED_TRACE(patterned ? "pattern" : "solid");
        std::string styleJSON = R"({
            "version": 8,
            "projection": {"type": "globe"},
            "center": [0, -0.095],
            "zoom": 12.5,
            "pitch": 60,
            "bearing": 33,
            "sources": {
                "building": {
                    "type": "geojson",
                    "data": {
                        "type": "FeatureCollection",
                        "features": [{"type":"Feature", "id":1,
                        "properties": {"base":30,"height":300,"color":"#ff0000","pattern":"checker"},
                        "geometry": {
                            "type": "Polygon",
                            "coordinates": [[[-0.1,-0.1],[-0.02,-0.1],[-0.02,0.1],[-0.1,0.1],[-0.1,-0.1]]]
                        }}, {"type":"Feature", "id":2,
                        "properties": {"base":60,"height":600,"color":"#00ff00","pattern":"checker_reverse"},
                        "geometry": {"type":"Polygon",
                            "coordinates": [[[0.02,-0.1],[0.1,-0.1],[0.1,0.1],[0.02,0.1],[0.02,-0.1]]]
                        }}]
                    }
                }
            },
            "layers": [
                {"id": "background", "type": "background", "paint": {"background-color": "#0000ff"}},
                {
                    "id": "building", "type": "fill-extrusion", "source": "building",
                    "paint": {
                        "fill-extrusion-base": ["get", "base"],
                        "fill-extrusion-height": ["coalesce", ["feature-state", "height"], ["get", "height"]],
                        "fill-extrusion-opacity": 0.8,
                        "fill-extrusion-translate": [7, -3],
                        "fill-extrusion-translate-anchor": "viewport",
                        "fill-extrusion-color": ["get", "color"])";
        if (patterned) {
            styleJSON += R"(, "fill-extrusion-pattern": ["get", "pattern"])";
        }
        styleJSON += "}}]}";
        map.getStyle().loadJSON(styleJSON);
        if (patterned) {
            PremultipliedImage pattern({8, 8});
            for (uint32_t y = 0; y < 8; ++y) {
                for (uint32_t x = 0; x < 8; ++x) {
                    auto* pixel = pattern.data.get() + 4 * (y * 8 + x);
                    const bool red = (x / 4 + y / 4) % 2 == 0;
                    pixel[0] = red ? 255 : 0;
                    pixel[1] = red ? 0 : 255;
                    pixel[2] = 0;
                    pixel[3] = 255;
                }
            }
            map.getStyle().addImage(std::make_unique<style::Image>("checker", std::move(pattern), 1.0f));
            PremultipliedImage reverse({8, 8});
            for (uint32_t y = 0; y < 8; ++y) {
                for (uint32_t x = 0; x < 8; ++x) {
                    auto* pixel = reverse.data.get() + 4 * (y * 8 + x);
                    const bool red = (x / 4 + y / 4) % 2 != 0;
                    pixel[0] = red ? 255 : 0;
                    pixel[1] = red ? 0 : 255;
                    pixel[2] = 0;
                    pixel[3] = 255;
                }
            }
            map.getStyle().addImage(std::make_unique<style::Image>("checker_reverse", std::move(reverse), 1.0f));
        }
        map.jumpTo(CameraOptions().withCenter(LatLng{-0.095, 0}).withPitch(60).withBearing(33));

        const std::array<double, 4> zooms{12.5, 11.5, 10.0, 12.0};
        std::array<PremultipliedImage, 4> referenceImages;
        const auto renderFrame = [&](const std::string& step) {
            const auto fullFramesBefore = frontend.fullFrames;
            const auto deferredFramesBefore = frontend.deferredFrames;
            auto image = renderStillImage(map, frontend, loop);
            const auto key = std::string(patterned ? "pattern_" : "solid_") + step;
            EXPECT_NE(frontend.fullFrameStats, nullptr);
            if (const auto& stats = frontend.fullFrameStats) {
                std::ostringstream values;
                values << "fullFramesBefore=" << fullFramesBefore << ",fullFramesAfter=" << frontend.fullFrames
                       << ",deferredFramesBefore=" << deferredFramesBefore
                       << ",deferredFramesAfter=" << frontend.deferredFrames
                       << ",totalBufferObjs=" << stats->totalBufferObjs
                       << ",bufferObjUpdates=" << stats->bufferObjUpdates
                       << ",uniformUpdateBytes=" << stats->uniformUpdateBytes
                       << ",numUniformUpdates=" << stats->numUniformUpdates
                       << ",numUniformBuffers=" << stats->numUniformBuffers
                       << ",memUniformBuffers=" << stats->memUniformBuffers << ",numDrawCalls=" << stats->numDrawCalls;
                RecordProperty(key.c_str(), values.str());
            }
            if (image.valid()) {
                const auto png = encodePNG(image);
                constexpr char digits[] = "0123456789abcdef";
                std::string hex;
                hex.reserve(png.size() * 2);
                for (const auto value : png) {
                    const auto byte = static_cast<uint8_t>(value);
                    hex += digits[byte >> 4];
                    hex += digits[byte & 15];
                }
                RecordProperty((key + "_png").c_str(), hex);
            }
            return image;
        };
        for (std::size_t i = 0; i < zooms.size(); ++i) {
            SCOPED_TRACE(::testing::Message() << "forward zoom " << zooms[i]);
            map.jumpTo(CameraOptions().withZoom(zooms[i]));
            auto image = renderFrame("forward_" + std::to_string(i));
            ASSERT_TRUE(image.valid());
            ASSERT_EQ(image.size, (mln::Size{512, 512}));
            std::size_t redPixels = 0;
            std::size_t greenPixels = 0;
            for (std::size_t offset = 0; offset < image.bytes(); offset += 4) {
                redPixels += image.data[offset] > image.data[offset + 2];
                greenPixels += image.data[offset + 1] > image.data[offset + 2];
            }
            EXPECT_GT(redPixels, 100u);
            EXPECT_GT(greenPixels, 100u);
            referenceImages[i] = std::move(image);
        }
        for (std::size_t i = zooms.size(); i-- > 0;) {
            SCOPED_TRACE(::testing::Message() << "return zoom " << zooms[i]);
            map.jumpTo(CameraOptions().withZoom(zooms[i]));
            const auto image = renderFrame("return_" + std::to_string(i));
            ASSERT_TRUE(image.valid());
            ASSERT_EQ(image.size, referenceImages[i].size);
            EXPECT_EQ(std::memcmp(image.data.get(), referenceImages[i].data.get(), image.bytes()), 0);
        }
        map.jumpTo(CameraOptions().withBearing(34));
        const auto movedImage = renderFrame("bearing_update");
        ASSERT_TRUE(movedImage.valid());
        ASSERT_EQ(movedImage.size, referenceImages[0].size);
        EXPECT_NE(std::memcmp(movedImage.data.get(), referenceImages[0].data.get(), movedImage.bytes()), 0);
        frontend.getRenderer()->setFeatureState("building", {}, "1", {{"height", 500.0}});
        const auto changedHeight = renderFrame("feature_state_update");
        ASSERT_TRUE(changedHeight.valid());
        ASSERT_EQ(changedHeight.size, movedImage.size);
        EXPECT_NE(std::memcmp(changedHeight.data.get(), movedImage.data.get(), movedImage.bytes()), 0);
        frontend.getRenderer()->removeFeatureState("building", {}, "1", "height");
        const auto restoredHeight = renderFrame("feature_state_restored");
        ASSERT_TRUE(restoredHeight.valid());
        ASSERT_EQ(restoredHeight.size, movedImage.size);
        EXPECT_EQ(std::memcmp(restoredHeight.data.get(), movedImage.data.get(), movedImage.bytes()), 0);
    }
    for (const auto shader : {shaders::BuiltIn::FillExtrusionShader,
                              shaders::BuiltIn::FillExtrusionInstancedShader,
                              shaders::BuiltIn::FillExtrusionPatternShader,
                              shaders::BuiltIn::FillExtrusionPatternInstancedShader}) {
        SCOPED_TRACE(static_cast<unsigned>(shader));
        const auto variants = frontend.shaderVariants.find(shader);
        ASSERT_NE(variants, frontend.shaderVariants.end());
        EXPECT_EQ(variants->second, 3u);
        RecordProperty(("shader_" + std::to_string(static_cast<unsigned>(shader)) + "_variants").c_str(),
                       static_cast<int>(variants->second));
    }
}

TEST(MetalStillRender, SymbolVariantsKeepPixelsAndRebuildResourcesAcrossProjectionRoundTrips) {
    auto pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
    auto device = NS::TransferPtr(MTL::CreateSystemDefaultDevice());
    if (!device) GTEST_SKIP() << "Metal device is unavailable";
    util::RunLoop loop;
    RecordingStillFrontend frontend;
    auto fileSource = std::make_shared<StubFileSource>(StubFileSource::ResponseType::Synchronous);
    MapAdapter map(frontend,
                   MapObserver::nullObserver(),
                   fileSource,
                   MapOptions().withMapMode(MapMode::Static).withSize(mln::Size{96, 96}));
    map.getStyle().loadJSON(R"({
        "version":8,"projection":{"type":"globe"},"center":[0,0],"zoom":12.5,"pitch":60,"bearing":33,
        "sources":{"point":{"type":"geojson","maxzoom":0,"data":{"type":"Point","coordinates":[0,0]}}},
        "layers":[
            {"id":"background","type":"background","paint":{"background-color":"#0000ff"}},
            {"id":"rgba","type":"symbol","source":"point","layout":{
                "icon-image":"marker","icon-offset":[-24,0],"icon-pitch-alignment":"viewport",
                "icon-rotation-alignment":"viewport","icon-allow-overlap":true,"icon-ignore-placement":true}},
            {"id":"sdf","type":"symbol","source":"point","layout":{
                "icon-image":"sdf","icon-pitch-alignment":"viewport","icon-rotation-alignment":"viewport",
                "icon-allow-overlap":true,"icon-ignore-placement":true},"paint":{"icon-color":"#00ff00"}},
            {"id":"formatted","type":"symbol","source":"point","layout":{
                "text-field":["format",["image","inline"],{}],"text-font":["Test Stack"],"text-size":24,
                "text-offset":[1,0],"text-pitch-alignment":"viewport","text-rotation-alignment":"viewport",
                "text-allow-overlap":true,"text-ignore-placement":true}}
        ]
    })");
    addRedIcon(map.getStyle());
    PremultipliedImage inlineImage({16, 16});
    for (std::size_t i = 0; i < inlineImage.bytes(); i += 4) {
        inlineImage.data[i] = 255;
        inlineImage.data[i + 1] = 255;
        inlineImage.data[i + 2] = 0;
        inlineImage.data[i + 3] = 255;
    }
    map.getStyle().addImage(std::make_unique<style::Image>("inline", std::move(inlineImage), 1.0f));
    PremultipliedImage sdf({16, 16});
    std::memset(sdf.data.get(), 255, sdf.bytes());
    map.getStyle().addImage(std::make_unique<style::Image>("sdf", std::move(sdf), 1.0f, true));
    std::map<double, PremultipliedImage> references;
    const std::array<double, 7> zooms{12.5, 11.5, 10.0, 12.0, 10.0, 11.5, 12.5};
    for (std::size_t step = 0; step < zooms.size(); ++step) {
        SCOPED_TRACE(::testing::Message() << "step " << step << " zoom " << zooms[step]);
        map.jumpTo(CameraOptions().withZoom(zooms[step]));
        for (int frame = 0; frame < 2; ++frame) {
            const auto fullFramesBefore = frontend.fullFrames;
            const auto deferredFramesBefore = frontend.deferredFrames;
            auto image = renderStillImage(map, frontend, loop);
            ASSERT_TRUE(image.valid());
            ASSERT_NE(frontend.fullFrameStats, nullptr);
            const auto& stats = *frontend.fullFrameStats;
            const auto key = "step_" + std::to_string(step) + "_frame_" + std::to_string(frame);
            std::ostringstream values;
            values << "fullFramesBefore=" << fullFramesBefore << ",fullFramesAfter=" << frontend.fullFrames
                   << ",deferredFramesBefore=" << deferredFramesBefore
                   << ",deferredFramesAfter=" << frontend.deferredFrames << ",numFrames=" << stats.numFrames
                   << ",totalBufferObjs=" << stats.totalBufferObjs << ",bufferObjUpdates=" << stats.bufferObjUpdates
                   << ",uniformUpdateBytes=" << stats.uniformUpdateBytes
                   << ",numUniformUpdates=" << stats.numUniformUpdates
                   << ",numUniformBuffers=" << stats.numUniformBuffers
                   << ",memUniformBuffers=" << stats.memUniformBuffers << ",numDrawCalls=" << stats.numDrawCalls;
            RecordProperty(key, values.str());
            std::size_t red = 0;
            std::size_t green = 0;
            std::size_t yellow = 0;
            for (std::size_t i = 0; i < image.bytes(); i += 4) {
                red += image.data[i] > 128 && image.data[i + 1] < 128 && image.data[i + 2] < 128;
                green += image.data[i] < 128 && image.data[i + 1] > 128 && image.data[i + 2] < 128;
                yellow += image.data[i] > 128 && image.data[i + 1] > 128 && image.data[i + 2] < 128;
            }
            EXPECT_GT(red, 100u);
            EXPECT_GT(green, 100u);
            EXPECT_GT(yellow, 100u);
            const auto png = encodePNG(image);
            constexpr char digits[] = "0123456789abcdef";
            std::string hex;
            for (const auto value : png) {
                const auto byte = static_cast<uint8_t>(value);
                hex += digits[byte >> 4];
                hex += digits[byte & 15];
            }
            RecordProperty(key + "_png", hex);
            if (const auto previous = references.find(zooms[step]); previous != references.end()) {
                ASSERT_EQ(image.size, previous->second.size);
                EXPECT_EQ(std::memcmp(image.data.get(), previous->second.data.get(), image.bytes()), 0);
            } else {
                references.emplace(zooms[step], std::move(image));
            }
        }
    }
    for (const auto shader : {shaders::BuiltIn::SymbolIconShader,
                              shaders::BuiltIn::SymbolSDFShader,
                              shaders::BuiltIn::SymbolTextAndIconShader}) {
        const auto variants = frontend.shaderVariants.find(shader);
        ASSERT_NE(variants, frontend.shaderVariants.end());
        EXPECT_EQ(variants->second, 3u);
        RecordProperty("shader_" + std::to_string(static_cast<unsigned>(shader)), static_cast<int>(variants->second));
    }
}

#endif
