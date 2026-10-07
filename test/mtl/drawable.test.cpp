#if MLN_RENDER_BACKEND_METAL

#include <mln/test/util.hpp>

#include <mln/gfx/backend_scope.hpp>
#include <mln/gfx/index_vector.hpp>
#include <mln/gfx/vertex_vector.hpp>
#include <mln/mtl/command_encoder.hpp>
#include <mln/mtl/context.hpp>
#include <mln/mtl/drawable.hpp>
#include <mln/mtl/drawable_impl.hpp>
#include <mln/mtl/headless_backend.hpp>
#include <mln/mtl/renderable_resource.hpp>
#include <mln/mtl/vertex_attribute.hpp>
#include <mln/mtl/vertex_buffer_resource.hpp>
#include <mln/shaders/shader_program_base.hpp>
#include <mln/util/run_loop.hpp>

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <array>

using namespace mln;

namespace {

class UploadShader final : public gfx::ShaderProgramBase {
public:
    enum class Input {
        Vertex,
        Instance,
        Both
    };

    explicit UploadShader(Input input = Input::Vertex, int bufferIndex = 8) {
        if (input != Input::Instance) {
            vertices.set(0, 0, gfx::AttributeDataType::Float2, bufferIndex);
        }
        if (input != Input::Vertex) {
            const int index = input == Input::Both ? 1 : 0;
            instances.set(0, index, gfx::AttributeDataType::Float2, bufferIndex + index);
        }
    }

    const std::string_view typeName() const noexcept override { return "UploadShader"; }
    std::optional<std::size_t> getSamplerLocation(std::size_t) const override { return std::nullopt; }
    const gfx::VertexAttributeArray& getVertexAttributes() const override { return vertices; }
    const gfx::VertexAttributeArray& getInstanceAttributes() const override { return instances; }

private:
    mtl::VertexAttributeArray vertices;
    mtl::VertexAttributeArray instances;
};

class ObservableDrawable final : public mtl::Drawable {
public:
    ObservableDrawable()
        : mtl::Drawable("AttributeUploadTest") {}

    const mtl::MTLVertexDescriptorPtr& descriptor() const { return impl->vertexDesc; }
    std::size_t descriptorHash() const { return impl->vertexDescHash; }
    const gfx::AttributeBindingArray& bindings() const { return impl->attributeBindings; }
    const gfx::AttributeBindingArray& instanceBindings() const { return impl->instanceBindings; }
};

class MetalDrawable : public testing::Test {
protected:
    using Vertex = std::array<float, 4>;

    void SetUp() override {
        pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
        auto device = NS::TransferPtr(MTL::CreateSystemDefaultDevice());
        if (!device) {
            GTEST_SKIP() << "Metal device is unavailable";
        }
        backend = std::make_unique<mtl::HeadlessBackend>(mln::Size{16, 16});
        scope = std::make_unique<gfx::BackendScope>(*backend);
        values = makeVertices();
        attributes = context().createVertexAttributeArray();
        setSharedAttribute(*attributes, values);
        drawable = std::make_unique<ObservableDrawable>();
        drawable->setShader(std::make_shared<UploadShader>());
        drawable->setVertexAttributes(attributes);
        drawable->setVertices({}, values->elements(), gfx::AttributeDataType::Float2);
        auto indexes = std::make_shared<gfx::IndexVector<gfx::Triangles>>();
        indexes->emplace_back(0, 1, 2);
        drawable->setIndexData(std::move(indexes), {});
    }

    void TearDown() override {
        drawable.reset();
        attributes.reset();
        values.reset();
        if (backend) {
            backend->getDefaultRenderable().getResource<mtl::RenderableResource>().swap();
        }
        scope.reset();
        backend.reset();
        pool.reset();
    }

    static std::shared_ptr<gfx::VertexVector<Vertex>> makeVertices() {
        auto result = std::make_shared<gfx::VertexVector<Vertex>>();
        result->extend(1024, Vertex{1, 2, 3, 4});
        result->updateModified();
        return result;
    }

    static void setSharedAttribute(gfx::VertexAttributeArray& target,
                                   const std::shared_ptr<gfx::VertexVector<Vertex>>& source,
                                   uint32_t offset = 0,
                                   uint32_t vertexOffset = 0,
                                   uint32_t stride = sizeof(Vertex),
                                   gfx::AttributeDataType type = gfx::AttributeDataType::Float2) {
        const auto& attribute = target.get(0) ? target.get(0) : target.set(0);
        attribute->setSharedRawData(source, offset, vertexOffset, stride, type);
    }

    void upload(ObservableDrawable& target) {
        auto encoder = context().createCommandEncoder();
        auto pass = encoder->createUploadPass("AttributeUploadTest", backend->getDefaultRenderable());
        target.upload(*pass);
    }

    void upload() { upload(*drawable); }

    mtl::Context& context() { return static_cast<mtl::Context&>(backend->getContext()); }

    util::RunLoop loop;
    NS::SharedPtr<NS::AutoreleasePool> pool;
    std::unique_ptr<mtl::HeadlessBackend> backend;
    std::unique_ptr<gfx::BackendScope> scope;
    std::shared_ptr<gfx::VertexVector<Vertex>> values;
    gfx::VertexAttributeArrayPtr attributes;
    std::unique_ptr<ObservableDrawable> drawable;
};

} // namespace

TEST_F(MetalDrawable, VertexDataUpdateKeepsLayoutAndPreservesThePreviousBuffer) {
    upload();
    const auto descriptor = drawable->descriptor();
    const auto hash = drawable->descriptorHash();
    ASSERT_TRUE(descriptor);
    ASSERT_TRUE(drawable->bindings().at(0));
    const auto* oldResource = static_cast<const mtl::VertexBufferResource*>(
        drawable->bindings().at(0)->vertexBufferResource);
    ASSERT_NE(oldResource, nullptr);
    const auto oldBuffer = oldResource->get().getMetalBuffer();
    ASSERT_TRUE(oldBuffer);

    values->at(0)[0] = 9;
    values->updateModified();
    upload();

    EXPECT_EQ(drawable->descriptor().get(), descriptor.get());
    EXPECT_EQ(drawable->descriptorHash(), hash);
    ASSERT_TRUE(drawable->bindings().at(0));
    const auto* resource = static_cast<const mtl::VertexBufferResource*>(
        drawable->bindings().at(0)->vertexBufferResource);
    ASSERT_NE(resource, nullptr);
    EXPECT_FLOAT_EQ(static_cast<const float*>(resource->contents())[0], 9.0f);
    EXPECT_NE(resource->get().getMetalBuffer().get(), oldBuffer.get());
    EXPECT_FLOAT_EQ(static_cast<const float*>(oldBuffer->contents())[0], 1.0f);
}

TEST_F(MetalDrawable, InstanceDataUpdateKeepsTheLayout) {
    auto instances = context().createVertexAttributeArray();
    const auto instanceValues = makeVertices();
    setSharedAttribute(*instances, instanceValues);
    drawable->setInstanceAttributes(instances);
    drawable->setShader(std::make_shared<UploadShader>(UploadShader::Input::Both));
    upload();
    const auto descriptor = drawable->descriptor();
    ASSERT_TRUE(descriptor);

    instanceValues->at(0)[0] = 7;
    instanceValues->updateModified();
    upload();

    EXPECT_EQ(drawable->descriptor().get(), descriptor.get());
    ASSERT_TRUE(drawable->instanceBindings().at(1));
    const auto* resource = static_cast<const mtl::VertexBufferResource*>(
        drawable->instanceBindings().at(1)->vertexBufferResource);
    ASSERT_NE(resource, nullptr);
    EXPECT_FLOAT_EQ(static_cast<const float*>(resource->contents())[0], 7.0f);
}

TEST_F(MetalDrawable, SharedDataSourceReplacementReachesEveryDrawable) {
    const auto replacement = makeVertices();
    replacement->at(0)[0] = 9;
    replacement->updateModified();
    auto second = std::make_unique<ObservableDrawable>();
    second->setShader(std::make_shared<UploadShader>());
    second->setVertexAttributes(attributes);
    second->setVertices({}, values->elements(), gfx::AttributeDataType::Float2);
    auto indexes = std::make_shared<gfx::IndexVector<gfx::Triangles>>();
    indexes->emplace_back(0, 1, 2);
    second->setIndexData(std::move(indexes), {});
    upload();
    upload(*second);

    setSharedAttribute(*attributes, replacement);
    upload();
    upload(*second);

    ASSERT_TRUE(second->bindings().at(0));
    const auto* resource = static_cast<const mtl::VertexBufferResource*>(
        second->bindings().at(0)->vertexBufferResource);
    ASSERT_NE(resource, nullptr);
    EXPECT_FLOAT_EQ(static_cast<const float*>(resource->contents())[0], 9.0f);
}

TEST_F(MetalDrawable, VertexOffsetUpdateKeepsTheLayout) {
    upload();
    const auto descriptor = drawable->descriptor();
    const auto hash = drawable->descriptorHash();

    setSharedAttribute(*attributes, values, 0, 1);
    upload();

    EXPECT_EQ(drawable->descriptor().get(), descriptor.get());
    EXPECT_EQ(drawable->descriptorHash(), hash);
    ASSERT_TRUE(drawable->bindings().at(0));
    EXPECT_EQ(drawable->bindings().at(0)->vertexOffset, 1u);
}

TEST_F(MetalDrawable, AttributeOffsetChangeRebuildsTheLayout) {
    upload();
    const auto descriptor = drawable->descriptor();
    const auto hash = drawable->descriptorHash();

    setSharedAttribute(*attributes, values, 8);
    upload();

    EXPECT_NE(drawable->descriptor().get(), descriptor.get());
    EXPECT_NE(drawable->descriptorHash(), hash);
    EXPECT_EQ(drawable->descriptor()->attributes()->object(0)->offset(), 8u);
}

TEST_F(MetalDrawable, AttributeStrideChangeRebuildsTheLayout) {
    upload();
    const auto descriptor = drawable->descriptor();

    setSharedAttribute(*attributes, values, 0, 0, 8);
    upload();

    EXPECT_NE(drawable->descriptor().get(), descriptor.get());
    EXPECT_EQ(drawable->descriptor()->layouts()->object(8)->stride(), 8u);
}

TEST_F(MetalDrawable, AttributeFormatChangeRebuildsTheLayout) {
    upload();
    const auto descriptor = drawable->descriptor();

    setSharedAttribute(*attributes, values, 0, 0, sizeof(Vertex), gfx::AttributeDataType::Float4);
    upload();

    EXPECT_NE(drawable->descriptor().get(), descriptor.get());
    EXPECT_EQ(drawable->descriptor()->attributes()->object(0)->format(), MTL::VertexFormatFloat4);
}

TEST_F(MetalDrawable, RemovingAnAttributeUsesAConstantBinding) {
    upload();
    const auto descriptor = drawable->descriptor();
    attributes->clear();

    upload();

    EXPECT_NE(drawable->descriptor().get(), descriptor.get());
    ASSERT_TRUE(drawable->bindings().at(0));
    EXPECT_EQ(drawable->bindings().at(0)->vertexBufferResource, nullptr);
    EXPECT_EQ(drawable->descriptor()->layouts()->object(8)->stepFunction(), MTL::VertexStepFunctionConstant);
    EXPECT_EQ(drawable->descriptor()->layouts()->object(8)->stepRate(), 0u);
}

TEST_F(MetalDrawable, RestoringAnAttributeReplacesTheConstantBinding) {
    attributes->clear();
    upload();
    const auto descriptor = drawable->descriptor();

    setSharedAttribute(*attributes, values);
    upload();

    EXPECT_NE(drawable->descriptor().get(), descriptor.get());
    ASSERT_TRUE(drawable->bindings().at(0));
    EXPECT_NE(drawable->bindings().at(0)->vertexBufferResource, nullptr);
    EXPECT_EQ(drawable->descriptor()->layouts()->object(8)->stepFunction(), MTL::VertexStepFunctionPerVertex);
    EXPECT_EQ(drawable->descriptor()->layouts()->object(8)->stepRate(), 1u);
}

TEST_F(MetalDrawable, ShaderBufferIndexChangeInvalidatesTheLayoutHash) {
    upload();
    const auto descriptor = drawable->descriptor();
    const auto hash = drawable->descriptorHash();

    drawable->setShader(std::make_shared<UploadShader>(UploadShader::Input::Vertex, 9));
    upload();

    EXPECT_NE(drawable->descriptor().get(), descriptor.get());
    EXPECT_NE(drawable->descriptorHash(), hash);
    EXPECT_EQ(drawable->descriptor()->attributes()->object(0)->bufferIndex(), 9u);
}

TEST_F(MetalDrawable, ChangingFromVertexToInstanceInputInvalidatesTheLayoutHash) {
    upload();
    const auto descriptor = drawable->descriptor();
    const auto hash = drawable->descriptorHash();
    drawable->setInstanceAttributes(attributes);
    drawable->setVertexAttributes(context().createVertexAttributeArray());
    drawable->setShader(std::make_shared<UploadShader>(UploadShader::Input::Instance));

    upload();

    EXPECT_NE(drawable->descriptor().get(), descriptor.get());
    EXPECT_NE(drawable->descriptorHash(), hash);
    EXPECT_EQ(drawable->descriptor()->layouts()->object(8)->stepFunction(), MTL::VertexStepFunctionPerInstance);
    EXPECT_EQ(drawable->descriptor()->layouts()->object(8)->stepRate(), 1u);
}

#endif
