#include <mln/shaders/mtl/shader_program.hpp>

#include <mln/gfx/render_pass.hpp>
#include <mln/mtl/context.hpp>
#include <mln/mtl/renderer_backend.hpp>
#include <mln/mtl/renderable_resource.hpp>
#include <mln/mtl/uniform_buffer.hpp>
#include <mln/mtl/vertex_attribute.hpp>
#include <mln/shaders/program_parameters.hpp>
#include <mln/shaders/shader_manifest.hpp>
#include <mln/util/logging.hpp>
#include <mln/util/hash.hpp>

#include <Metal/MTLLibrary.hpp>
#include <Metal/MTLRenderPass.hpp>
#include <Metal/MTLRenderPipeline.hpp>

#include <cstring>
#include <utility>
#include <algorithm>

using namespace std::string_literals;

namespace mln {

namespace mtl {
namespace {
MTL::BlendOperation metalBlendOperation(const gfx::ColorBlendEquationType& colorBlend) {
    switch (colorBlend) {
        case gfx::ColorBlendEquationType::Add:
            return MTL::BlendOperationAdd;
        case gfx::ColorBlendEquationType::Subtract:
            return MTL::BlendOperationSubtract;
        case gfx::ColorBlendEquationType::ReverseSubtract:
            return MTL::BlendOperationReverseSubtract;
    }
}

MTL::BlendFactor metalBlendFactor(const gfx::ColorBlendFactorType& colorFactor) {
    switch (colorFactor) {
        case gfx::ColorBlendFactorType::Zero:
            return MTL::BlendFactorZero;
        case gfx::ColorBlendFactorType::One:
            return MTL::BlendFactorOne;
        case gfx::ColorBlendFactorType::SrcColor:
            return MTL::BlendFactorSourceColor;
        case gfx::ColorBlendFactorType::OneMinusSrcColor:
            return MTL::BlendFactorOneMinusSourceColor;
        case gfx::ColorBlendFactorType::SrcAlpha:
            return MTL::BlendFactorSourceAlpha;
        case gfx::ColorBlendFactorType::OneMinusSrcAlpha:
            return MTL::BlendFactorOneMinusSourceAlpha;
        case gfx::ColorBlendFactorType::DstAlpha:
            return MTL::BlendFactorDestinationAlpha;
        case gfx::ColorBlendFactorType::OneMinusDstAlpha:
            return MTL::BlendFactorOneMinusDestinationAlpha;
        case gfx::ColorBlendFactorType::DstColor:
            return MTL::BlendFactorDestinationColor;
        case gfx::ColorBlendFactorType::OneMinusDstColor:
            return MTL::BlendFactorOneMinusDestinationColor;
        case gfx::ColorBlendFactorType::SrcAlphaSaturate:
            return MTL::BlendFactorSourceAlphaSaturated;
        case gfx::ColorBlendFactorType::ConstantColor:
            return MTL::BlendFactorBlendColor;
        case gfx::ColorBlendFactorType::OneMinusConstantColor:
            return MTL::BlendFactorOneMinusBlendColor;
        case gfx::ColorBlendFactorType::ConstantAlpha:
            return MTL::BlendFactorBlendAlpha;
        case gfx::ColorBlendFactorType::OneMinusConstantAlpha:
            return MTL::BlendFactorOneMinusBlendAlpha;
    }
}
} // namespace

ShaderProgram::ShaderProgram(std::string name,
                             RendererBackend& backend_,
                             std::future<Functions> functions,
                             shaders::BuiltIn shaderID_,
                             std::string defines_)
    : ShaderProgramBase(),
      shaderName(std::move(name)),
      backend(backend_),
      pendingFunctions(std::move(functions)),
      shaderID(shaderID_),
      defines(std::move(defines_)) {}

ShaderProgram::~ShaderProgram() noexcept = default;

MTLRenderPipelineStatePtr ShaderProgram::getRenderPipelineState(const gfx::Renderable& renderable,
                                                                const MTLVertexDescriptorPtr& vertexDescriptor,
                                                                const gfx::ColorMode& colorMode,
                                                                const std::size_t reuseHash) const {
    auto& context = static_cast<Context&>(backend.getContext());
    if (pendingFunctions.valid()) {
        if (pendingFunctions.wait_for(std::chrono::seconds::zero()) != std::future_status::ready) {
            context.deferRendering();
            return {};
        }
        try {
            auto functions = pendingFunctions.get();
            vertexFunction = std::move(functions.vertex);
            fragmentFunction = std::move(functions.fragment);
            if (!vertexFunction) {
                throw std::runtime_error(shaderName + " missing vertex function");
            }
        } catch (const std::exception& error) {
            Log::Error(Event::Shader, error.what());
            compilationError = std::current_exception();
        }
        context.shaderCompilationFinished(shaderID, defines, compilationError);
    }
    if (compilationError) {
        context.reportRenderError(compilationError);
        return {};
    }

    const auto& renderableResource = renderable.getResource<RenderableResource>();

    auto colorFormat = MTL::PixelFormat::PixelFormatBGRA8Unorm;
    std::optional<MTL::PixelFormat> depthFormat = std::nullopt;
    std::optional<MTL::PixelFormat> stencilFormat = std::nullopt;
    if (const auto& rpd = renderableResource.getRenderPassDescriptor()) {
        if (auto* colorTarget = rpd->colorAttachments()->object(0)) {
            if (auto* tex = colorTarget->texture()) {
                colorFormat = tex->pixelFormat();
            }
        }
        if (auto* depthTarget = rpd->depthAttachment()) {
            if (auto* tex = depthTarget->texture()) {
                depthFormat = tex->pixelFormat();
            }
        }
        if (auto* stencilTarget = rpd->stencilAttachment()) {
            if (auto* tex = stencilTarget->texture()) {
                stencilFormat = tex->pixelFormat();
            }
        }
    }

    const auto key = util::hash(reuseHash,
                                static_cast<std::size_t>(colorFormat),
                                static_cast<std::size_t>(depthFormat.value_or(MTL::PixelFormatInvalid)),
                                static_cast<std::size_t>(stencilFormat.value_or(MTL::PixelFormatInvalid)));
    if (auto it = renderPipelineStateCache.find(key); it != renderPipelineStateCache.end()) {
        auto& pipeline = it->second;
        if (pipeline.pending.valid()) {
            if (pipeline.pending.wait_for(std::chrono::seconds::zero()) != std::future_status::ready) {
                context.deferRendering();
                return {};
            }
            try {
                pipeline.ready = pipeline.pending.get();
            } catch (const std::exception& error) {
                pipeline.error = std::current_exception();
                Log::Error(Event::Shader, error.what());
            }
        }
        if (pipeline.error) {
            context.reportRenderError(pipeline.error);
        }
        return pipeline.ready;
    }

    auto pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
    auto desc = NS::TransferPtr(MTL::RenderPipelineDescriptor::alloc()->init());
    desc->setLabel(NS::String::string(shaderName.data(), NS::UTF8StringEncoding));
    desc->setVertexFunction(vertexFunction.get());
    desc->setFragmentFunction(fragmentFunction.get());
    desc->setVertexDescriptor(vertexDescriptor.get());

    if (auto* colorTarget = desc->colorAttachments()->object(0)) {
        const auto blendEnabled = !colorMode.blendFunction.is<gfx::ColorMode::Replace>();
        auto blendOperation = MTL::BlendOperationAdd;
        auto srcFactor = MTL::BlendFactorOne;
        auto destFactor = MTL::BlendFactorOne;

        if (blendEnabled) {
            apply_visitor(
                [&](const auto& blendFunction) {
                    blendOperation = metalBlendOperation(gfx::ColorBlendEquationType(blendFunction.equation));
                    srcFactor = metalBlendFactor(blendFunction.srcFactor);
                    destFactor = metalBlendFactor(blendFunction.dstFactor);
                },
                colorMode.blendFunction);
        }

        colorTarget->setPixelFormat(colorFormat);
        colorTarget->setBlendingEnabled(blendEnabled);
        colorTarget->setRgbBlendOperation(blendOperation);
        colorTarget->setAlphaBlendOperation(blendOperation);
        colorTarget->setSourceRGBBlendFactor(srcFactor);
        colorTarget->setSourceAlphaBlendFactor(srcFactor);
        colorTarget->setDestinationRGBBlendFactor(destFactor);
        colorTarget->setDestinationAlphaBlendFactor(destFactor);

        colorTarget->setWriteMask((colorMode.mask.r ? MTL::ColorWriteMaskRed : MTL::ColorWriteMaskNone) |
                                  (colorMode.mask.g ? MTL::ColorWriteMaskGreen : MTL::ColorWriteMaskNone) |
                                  (colorMode.mask.b ? MTL::ColorWriteMaskBlue : MTL::ColorWriteMaskNone) |
                                  (colorMode.mask.a ? MTL::ColorWriteMaskAlpha : MTL::ColorWriteMaskNone));
    }

    if (depthFormat) {
        desc->setDepthAttachmentPixelFormat(*depthFormat);
    }

    if (stencilFormat) {
        desc->setStencilAttachmentPixelFormat(*stencilFormat);
    }

    auto promise = std::make_shared<std::promise<MTLRenderPipelineStatePtr>>();
    renderPipelineStateCache.emplace(key, PipelineState{promise->get_future(), {}, nullptr});
    context.deferRendering();
    auto completion = [promise, wake = context.shaderCompilationCallback(), name = shaderName](
                          MTL::RenderPipelineState* pipeline, NS::Error* error) {
        if (pipeline) {
            promise->set_value(NS::RetainPtr(pipeline));
        } else {
            const auto* description = error ? error->localizedDescription()->utf8String() : nullptr;
            promise->set_exception(std::make_exception_ptr(std::runtime_error(
                name + " newRenderPipelineState failed" + (description ? ": "s + description : ""s))));
        }
        wake();
    };
    const auto& device = backend.getDevice();
    device->newRenderPipelineState(desc.get(),
                                   MTL::NewRenderPipelineStateCompletionHandlerFunction(std::move(completion)));
    return {};
}

std::optional<size_t> ShaderProgram::getSamplerLocation(const size_t id) const {
    return (id < textureBindings.size()) ? textureBindings[id] : std::nullopt;
}

void ShaderProgram::initVertexAttribute(const shaders::AttributeInfo& info) {
    const auto index = static_cast<int>(info.index);
    const auto bufferIndex = static_cast<int>(info.bufferIndex);
#if !defined(NDEBUG)
    // Indexes must be unique, if there's a conflict check the `attributes` array in the shader
    vertexAttributes.visitAttributes([&](const gfx::VertexAttribute& attrib) { assert(attrib.getIndex() != index); });
#endif
    vertexAttributes.set(info.id, index, info.dataType, bufferIndex);
}

void ShaderProgram::initInstanceAttribute(const shaders::AttributeInfo& info) {
    // Index is the block index of the instance attribute
    const auto index = static_cast<int>(info.index);
    const auto bufferIndex = static_cast<int>(info.bufferIndex);
#if !defined(NDEBUG)
    // Indexes must not be reused by regular attributes or uniform blocks
    // More than one instance attribute can have the same index, if they share the block
    instanceAttributes.visitAttributes([&](const gfx::VertexAttribute& attrib) { assert(attrib.getIndex() != index); });
#endif
    instanceAttributes.set(info.id, index, info.dataType, bufferIndex);
}

void ShaderProgram::initTexture(const shaders::TextureInfo& info) {
    assert(info.id < textureBindings.size());
    if (info.id >= textureBindings.size()) {
        return;
    }
    textureBindings[info.id] = info.index;
}

} // namespace mtl
} // namespace mln
