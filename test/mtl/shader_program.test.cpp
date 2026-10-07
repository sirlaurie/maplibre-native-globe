#if MLN_RENDER_BACKEND_METAL

#include <mln/test/util.hpp>

#include <mln/gfx/backend_scope.hpp>
#include <mln/gfx/context_observer.hpp>
#include <mln/gfx/offscreen_texture.hpp>
#include <mln/mtl/context.hpp>
#include <mln/mtl/headless_backend.hpp>
#include <mln/mtl/renderable_resource.hpp>
#include <mln/shaders/mtl/fill.hpp>
#include <mln/shaders/mtl/line.hpp>
#include <mln/shaders/mtl/shader_group.hpp>
#include <mln/shaders/mtl/symbol.hpp>
#include <mln/util/run_loop.hpp>
#include <mln/util/timer.hpp>

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <future>
#include <stdexcept>
#include <thread>

using namespace mln;

namespace {

constexpr auto testShader = R"(
#include <metal_stdlib>
using namespace metal;

vertex float4 vertexMain(uint id [[vertex_id]]) {
    return float4(float(id), 0.0, 0.0, 1.0);
}

fragment float4 fragmentMain() {
    return float4(1.0, 0.0, 0.0, 1.0);
}
)";

class CompilationObserver final : public gfx::ContextObserver {
public:
    void onPreCompileShader(shaders::BuiltIn, gfx::Backend::Type backend, const std::string&) override {
        EXPECT_EQ(backend, gfx::Backend::Type::Metal);
        EXPECT_EQ(std::this_thread::get_id(), renderThread);
        ++started;
    }

    void onPostCompileShader(shaders::BuiltIn, gfx::Backend::Type backend, const std::string&) override {
        EXPECT_EQ(backend, gfx::Backend::Type::Metal);
        EXPECT_EQ(std::this_thread::get_id(), renderThread);
        ++finished;
    }

    void onShaderCompileFailed(shaders::BuiltIn, gfx::Backend::Type backend, const std::string&) override {
        EXPECT_EQ(backend, gfx::Backend::Type::Metal);
        EXPECT_EQ(std::this_thread::get_id(), renderThread);
        ++failed;
    }

    void onRenderError(std::exception_ptr error) override {
        EXPECT_EQ(std::this_thread::get_id(), renderThread);
        EXPECT_NE(error, nullptr);
        ++errors;
    }

    void onInvalidate() override {
        EXPECT_EQ(std::this_thread::get_id(), renderThread);
        ++invalidations;
        if (invalidated) {
            invalidated();
        }
    }

    const std::thread::id renderThread = std::this_thread::get_id();
    unsigned started = 0;
    unsigned finished = 0;
    unsigned failed = 0;
    unsigned errors = 0;
    unsigned invalidations = 0;
    std::function<void()> invalidated;
};

mtl::MTLVertexDescriptorPtr positionDescriptor(std::size_t bufferIndex, bool line = false) {
    auto descriptor = NS::TransferPtr(MTL::VertexDescriptor::alloc()->init());
    auto* position = descriptor->attributes()->object(0);
    position->setFormat(MTL::VertexFormatShort2);
    position->setBufferIndex(bufferIndex);
    position->setOffset(0);
    auto* layout = descriptor->layouts()->object(bufferIndex);
    layout->setStride(line ? 8 : 4);
    layout->setStepFunction(MTL::VertexStepFunctionPerVertex);
    layout->setStepRate(1);
    if (line) {
        auto* data = descriptor->attributes()->object(1);
        data->setFormat(MTL::VertexFormatUChar4);
        data->setBufferIndex(bufferIndex);
        data->setOffset(4);
    }
    return descriptor;
}

class MetalShaderCompilation : public testing::Test {
protected:
    void SetUp() override {
        pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
        auto device = NS::TransferPtr(MTL::CreateSystemDefaultDevice());
        if (!device) {
            GTEST_SKIP() << "Metal device is unavailable";
        }
        backend = std::make_unique<mtl::HeadlessBackend>(mln::Size{16, 16});
        scope = std::make_unique<gfx::BackendScope>(*backend);
        context().setObserver(&observer);
        backend->getDefaultRenderable().getResource<mtl::RenderableResource>().bind();
    }

    void TearDown() override {
        observer.invalidated = {};
        if (backend) {
            context().setObserver(nullptr);
            backend->getDefaultRenderable().getResource<mtl::RenderableResource>().swap();
        }
        scope.reset();
        backend.reset();
        pool.reset();
    }

    mtl::Context& context() { return static_cast<mtl::Context&>(backend->getContext()); }

    bool waitFor(const std::function<bool()>& completed) {
        bool done = completed();
        if (done) {
            return true;
        }
        observer.invalidated = [&] {
            if (completed()) {
                done = true;
                loop.stop();
            }
        };
        util::Timer deadline;
        deadline.start(Seconds(30), mln::Duration::zero(), [&] { loop.stop(); });
        loop.run();
        observer.invalidated = {};
        return done;
    }

    mtl::MTLRenderPipelineStatePtr waitForPipeline(mtl::ShaderProgram& shader,
                                                   const gfx::Renderable& renderable,
                                                   const mtl::MTLVertexDescriptorPtr& descriptor,
                                                   std::size_t key) {
        mtl::MTLRenderPipelineStatePtr pipeline;
        const auto completed = waitFor([&] {
            context().beginFrame();
            pipeline = shader.getRenderPipelineState(renderable, descriptor, gfx::ColorMode::unblended(), key);
            return pipeline || observer.errors > 0;
        });
        EXPECT_TRUE(completed) << "Metal compilation did not signal completion";
        return pipeline;
    }

    template <shaders::BuiltIn shaderID>
    void checkProjectionCaches(const gfx::StringIDSetsPair& uniforms, const mtl::MTLVertexDescriptorPtr& descriptor) {
        mtl::ShaderGroup<shaderID> group(ProgramParameters(1.0f, false));
        gfx::ShaderPtr mercator;
        for (const auto projection : {gfx::ProjectionVariant::Mercator, gfx::ProjectionVariant::Globe}) {
            SCOPED_TRACE(projection == gfx::ProjectionVariant::Globe ? "Globe" : "Mercator");
            const auto shader = group.getOrCreateShader(context(), uniforms, projection, "a_pos");
            ASSERT_NE(shader, nullptr);
            const auto cached = group.getOrCreateShader(context(), uniforms, projection, "a_pos");
            EXPECT_EQ(shader.get(), cached.get());
            if (projection == gfx::ProjectionVariant::Mercator) {
                mercator = shader;
            } else {
                EXPECT_NE(shader.get(), mercator.get());
            }
            auto* program = shader->template to<mtl::ShaderProgram>();
            ASSERT_NE(program, nullptr);
            auto pipeline = waitForPipeline(*program, backend->getDefaultRenderable(), descriptor, 17);
            ASSERT_TRUE(pipeline);
            context().beginFrame();
            auto reused = program->getRenderPipelineState(
                backend->getDefaultRenderable(), descriptor, gfx::ColorMode::unblended(), 17);
            EXPECT_EQ(pipeline.get(), reused.get());
            EXPECT_FALSE(context().isRenderingDeferred());
            EXPECT_FALSE(context().hasRenderingFailed());
        }
        EXPECT_EQ(observer.started, 2u);
        EXPECT_EQ(observer.finished, 2u);
        EXPECT_EQ(observer.failed, 0u);
        EXPECT_EQ(observer.errors, 0u);
        EXPECT_GT(observer.invalidations, 0u);
    }

    mtl::UniqueShaderProgram createProgram(std::string_view source, std::string_view vertexName = "vertexMain") {
        return context().createProgram(shaders::BuiltIn::None,
                                       "MetalCompilationTest",
                                       source,
                                       vertexName,
                                       "fragmentMain",
                                       ProgramParameters(1.0f, false),
                                       {});
    }

    void expectTerminalFailure(mtl::ShaderProgram& shader) {
        const auto descriptor = NS::TransferPtr(MTL::VertexDescriptor::alloc()->init());
        ASSERT_FALSE(waitForPipeline(shader, backend->getDefaultRenderable(), descriptor, 21));
        ASSERT_EQ(observer.errors, 1u);
        EXPECT_TRUE(context().hasRenderingFailed());
        for (unsigned request = 0; request < 2; ++request) {
            EXPECT_FALSE(shader.getRenderPipelineState(
                backend->getDefaultRenderable(), descriptor, gfx::ColorMode::unblended(), 21));
            EXPECT_TRUE(context().hasRenderingFailed());
            EXPECT_FALSE(context().isRenderingDeferred());
        }
        EXPECT_EQ(observer.started, 1u);
        EXPECT_EQ(observer.errors, 1u);
        context().beginFrame();
        EXPECT_FALSE(shader.getRenderPipelineState(
            backend->getDefaultRenderable(), descriptor, gfx::ColorMode::unblended(), 21));
        EXPECT_EQ(observer.errors, 2u);
        EXPECT_TRUE(context().hasRenderingFailed());
        EXPECT_FALSE(context().isRenderingDeferred());
        EXPECT_EQ(observer.started, 1u);
    }

    util::RunLoop loop;
    NS::SharedPtr<NS::AutoreleasePool> pool;
    CompilationObserver observer;
    std::unique_ptr<mtl::HeadlessBackend> backend;
    std::unique_ptr<gfx::BackendScope> scope;
};

TEST_F(MetalShaderCompilation, PendingFunctionsDeferRenderingWithoutWaitingForThePromise) {
    std::promise<mtl::ShaderProgram::Functions> functions;
    mtl::ShaderProgram shader("Pending", *backend, functions.get_future(), shaders::BuiltIn::None, "");
    const auto descriptor = NS::TransferPtr(MTL::VertexDescriptor::alloc()->init());

    context().beginFrame();
    EXPECT_FALSE(
        shader.getRenderPipelineState(backend->getDefaultRenderable(), descriptor, gfx::ColorMode::unblended(), 1));
    EXPECT_TRUE(context().isRenderingDeferred());
    EXPECT_FALSE(context().hasRenderingFailed());
    EXPECT_EQ(observer.finished, 0u);
    EXPECT_EQ(observer.failed, 0u);
    EXPECT_EQ(observer.errors, 0u);

    functions.set_value({});
}

TEST_F(MetalShaderCompilation, FillCachesMercatorAndGlobeProgramsAndPipelines) {
    checkProjectionCaches<shaders::BuiltIn::FillShader>({{"a_color", "a_opacity"}, {}},
                                                        positionDescriptor(shaders::fillUBOCount));
}

TEST_F(MetalShaderCompilation, LineCachesMercatorAndGlobeProgramsAndPipelines) {
    checkProjectionCaches<shaders::BuiltIn::LineShader>(
        {{"a_color", "a_blur", "a_opacity", "a_gapwidth", "a_offset", "a_width"}, {}},
        positionDescriptor(shaders::lineUBOCount, true));
}

TEST_F(MetalShaderCompilation, SymbolIconCachesMercatorAndGlobeProgramsAndPipelines) {
    checkProjectionCaches<shaders::BuiltIn::SymbolIconShader>({{"a_opacity", "a_sorted_instance"}, {}},
                                                              positionDescriptor(shaders::symbolUBOCount));
}

TEST_F(MetalShaderCompilation, SymbolSDFCachesMercatorAndGlobeProgramsAndPipelines) {
    checkProjectionCaches<shaders::BuiltIn::SymbolSDFShader>(
        {{"a_fill_color", "a_halo_color", "a_opacity", "a_halo_width", "a_halo_blur", "a_sorted_instance"}, {}},
        positionDescriptor(shaders::symbolUBOCount));
}

TEST_F(MetalShaderCompilation, InvalidSourceReportsOneErrorPerFrameWithoutRecompiling) {
    auto shader = createProgram("invalid metal source");
    ASSERT_NE(shader, nullptr);
    expectTerminalFailure(*shader);
    EXPECT_EQ(observer.failed, 1u);
    EXPECT_EQ(observer.finished, 0u);
}

TEST_F(MetalShaderCompilation, PipelineCacheSeparatesAttachmentFormatsWithTheSameReuseHash) {
    auto shader = createProgram(testShader);
    ASSERT_NE(shader, nullptr);
    const auto descriptor = NS::TransferPtr(MTL::VertexDescriptor::alloc()->init());
    struct Attachments {
        gfx::TextureChannelDataType color;
        bool depth;
        bool stencil;
    };
    const Attachments attachments[] = {
        {gfx::TextureChannelDataType::UnsignedByte, false, false},
        {gfx::TextureChannelDataType::HalfFloat, false, false},
        {gfx::TextureChannelDataType::UnsignedByte, true, false},
        {gfx::TextureChannelDataType::UnsignedByte, true, true},
        {gfx::TextureChannelDataType::UnsignedByte, false, true},
    };
    std::vector<mtl::MTLRenderPipelineStatePtr> pipelines;
    for (const auto& attachment : attachments) {
        auto target = context().createOffscreenTexture(
            mln::Size{8, 8}, attachment.color, attachment.depth, attachment.stencil);
        auto& resource = target->getResource<mtl::RenderableResource>();
        resource.bind();
        auto pipeline = waitForPipeline(*shader, *target, descriptor, 37);
        EXPECT_TRUE(pipeline);
        if (pipeline) {
            for (const auto& previous : pipelines) {
                EXPECT_NE(previous.get(), pipeline.get());
            }
            context().beginFrame();
            auto cached = shader->getRenderPipelineState(*target, descriptor, gfx::ColorMode::unblended(), 37);
            EXPECT_EQ(cached.get(), pipeline.get());
            EXPECT_FALSE(context().isRenderingDeferred());
            pipelines.push_back(pipeline);
        }
        resource.swap();
    }
    EXPECT_EQ(pipelines.size(), 5u);
    EXPECT_EQ(observer.started, 1u);
    EXPECT_EQ(observer.finished, 1u);
    EXPECT_EQ(observer.errors, 0u);
}

TEST_F(MetalShaderCompilation, MissingFunctionReportsOneErrorPerFrameWithoutRecompiling) {
    auto shader = createProgram(testShader, "missingVertex");
    ASSERT_NE(shader, nullptr);
    expectTerminalFailure(*shader);
    EXPECT_EQ(observer.failed, 1u);
    EXPECT_EQ(observer.finished, 0u);
}

TEST_F(MetalShaderCompilation, InvalidPipelineReportsOneErrorPerFrameWithoutRecompiling) {
    constexpr auto source = R"(
#include <metal_stdlib>
using namespace metal;
struct Vertex { float4 position [[attribute(0)]]; };
vertex float4 vertexMain(Vertex input [[stage_in]]) { return input.position; }
fragment float4 fragmentMain() { return float4(1.0); }
)";
    auto shader = createProgram(source);
    ASSERT_NE(shader, nullptr);
    expectTerminalFailure(*shader);
    EXPECT_EQ(observer.failed, 0u);
    EXPECT_EQ(observer.finished, 1u);
}

TEST_F(MetalShaderCompilation, CompletionAfterBackendDestructionDoesNotInvalidateTheObserver) {
    std::promise<mtl::ShaderProgram::Functions> functions;
    auto shader = std::make_unique<mtl::ShaderProgram>(
        "Pending destruction", *backend, functions.get_future(), shaders::BuiltIn::None, "");
    const auto descriptor = NS::TransferPtr(MTL::VertexDescriptor::alloc()->init());
    EXPECT_FALSE(
        shader->getRenderPipelineState(backend->getDefaultRenderable(), descriptor, gfx::ColorMode::unblended(), 1));
    auto completion = context().shaderCompilationCallback();
    shader.reset();
    backend->getDefaultRenderable().getResource<mtl::RenderableResource>().swap();
    scope.reset();
    backend.reset();

    bool barrierReached = false;
    std::thread compiler([&, completion = std::move(completion)] {
        functions.set_exception(std::make_exception_ptr(std::runtime_error("compiler finished")));
        completion();
        loop.invoke([&] {
            barrierReached = true;
            loop.stop();
        });
    });
    util::Timer deadline;
    deadline.start(Seconds(30), mln::Duration::zero(), [&] { loop.stop(); });
    loop.run();
    compiler.join();

    EXPECT_TRUE(barrierReached);
    EXPECT_EQ(observer.invalidations, 0u);
    EXPECT_EQ(observer.errors, 0u);
}

} // namespace

#endif
