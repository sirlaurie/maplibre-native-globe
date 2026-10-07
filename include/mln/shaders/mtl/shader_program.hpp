#pragma once

#include <mln/shaders/shader_program_base.hpp>
#include <mln/shaders/shader_source.hpp>
#include <mln/mtl/mtl_fwd.hpp>
#include <mln/mtl/vertex_attribute.hpp>

#include <Foundation/NSSharedPtr.hpp>

#include <optional>
#include <future>
#include <string>
#include <unordered_map>

namespace mln {
namespace shaders {

struct AttributeInfo {
    constexpr AttributeInfo(std::size_t index_,
                            gfx::AttributeDataType dataType_,
                            std::size_t bufferIndex_,
                            std::size_t id_)
        : index(index_),
          dataType(dataType_),
          bufferIndex(bufferIndex_),
          id(id_) {}
    std::size_t index;
    gfx::AttributeDataType dataType;
    std::size_t bufferIndex;
    std::size_t id;
};

struct TextureInfo {
    constexpr TextureInfo(std::size_t index_, std::size_t id_)
        : index(index_),
          id(id_) {}
    std::size_t index;
    std::size_t id;
};

} // namespace shaders
namespace mtl {
class RenderableResource;
class RendererBackend;
class ShaderProgram;
using UniqueShaderProgram = std::unique_ptr<ShaderProgram>;

class ShaderProgram final : public gfx::ShaderProgramBase {
public:
    struct Functions {
        MTLFunctionPtr vertex;
        MTLFunctionPtr fragment;
    };

    ShaderProgram(
        std::string name, RendererBackend& backend, std::future<Functions>, shaders::BuiltIn, std::string defines);
    ~ShaderProgram() noexcept override;

    static constexpr std::string_view Name{"GenericMTLShader"};
    const std::string_view typeName() const noexcept override { return Name; }

    MTLRenderPipelineStatePtr getRenderPipelineState(const gfx::Renderable&,
                                                     const MTLVertexDescriptorPtr&,
                                                     const gfx::ColorMode& colorMode,
                                                     std::size_t reuseHash) const;

    std::optional<size_t> getSamplerLocation(const size_t id) const override;

    const gfx::VertexAttributeArray& getVertexAttributes() const override { return vertexAttributes; }

    const gfx::VertexAttributeArray& getInstanceAttributes() const override { return instanceAttributes; }

    void initVertexAttribute(const shaders::AttributeInfo&);
    void initInstanceAttribute(const shaders::AttributeInfo&);
    void initTexture(const shaders::TextureInfo&);

protected:
    struct PipelineState {
        std::future<MTLRenderPipelineStatePtr> pending;
        MTLRenderPipelineStatePtr ready;
        std::exception_ptr error;
    };

    std::string shaderName;
    RendererBackend& backend;
    mutable std::future<Functions> pendingFunctions;
    const shaders::BuiltIn shaderID;
    const std::string defines;
    mutable MTLFunctionPtr vertexFunction;
    mutable MTLFunctionPtr fragmentFunction;
    mutable std::exception_ptr compilationError;
    VertexAttributeArray vertexAttributes;
    VertexAttributeArray instanceAttributes;
    std::array<std::optional<size_t>, shaders::maxTextureCountPerShader> textureBindings;

    mutable mln::unordered_map<std::size_t, PipelineState> renderPipelineStateCache;
};

} // namespace mtl
} // namespace mln
