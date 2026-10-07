#include <mln/test/util.hpp>

#include <mln/gfx/vertex_attribute.hpp>
#include <mln/gfx/vertex_vector.hpp>

#include <array>
#include <string_view>

using namespace mln;

namespace {

class FixedVertices final : public gfx::VertexVectorBase {
public:
    explicit FixedVertices(double modified = 1.0, std::size_t count_ = 4)
        : count(count_) {
        lastModified = std::chrono::duration<double>(modified);
    }

    const void* getRawData() const override { return values.data(); }
    std::size_t getRawSize() const override { return sizeof(float); }
    std::size_t getRawCount() const override { return count; }
    void setModificationTime(std::chrono::duration<double> time) { lastModified = time; }

private:
    std::array<float, 4> values{1, 2, 3, 4};
    std::size_t count;
};

class CountingAttributes final : public gfx::VertexAttributeArray {
public:
    mutable std::size_t creations = 0;

private:
    gfx::UniqueVertexAttribute create(int index, gfx::AttributeDataType type, std::size_t count) const override {
        ++creations;
        return gfx::VertexAttributeArray::create(index, type, count);
    }
};

struct SharedPaintProperty {
    using Attribute = gfx::AttributeType<float, 1>;
    static constexpr std::array<std::string_view, 1> AttributeNames{"a_value"};
};

struct SharedPaintBinder {
    std::shared_ptr<FixedVertices> data;

    std::size_t getVertexCount() const { return data->getRawCount(); }

    template <class>
    void applyPaintProperty(std::size_t, gfx::VertexAttribute& attribute) const {
        attribute.setSharedRawData(data, 0, 0, sizeof(float), gfx::AttributeDataType::Float);
    }
};

struct SharedPaintBinders {
    std::shared_ptr<SharedPaintBinder> binder;

    template <class>
    const auto& get() const {
        return binder;
    }
};

struct SharedPaintEvaluation {
    bool constant = false;

    bool isConstant() const noexcept { return constant; }

    template <class>
    SharedPaintEvaluation get() const {
        return *this;
    }
};

constexpr auto uploadedAt = std::chrono::duration<double>(10.0);
constexpr std::size_t paintAttributeID = 2;

void readPaint(CountingAttributes& attributes,
               const std::shared_ptr<FixedVertices>& data,
               bool constant = false,
               gfx::StringIDSetsPair* uniforms = nullptr) {
    SharedPaintBinders binders{std::make_shared<SharedPaintBinder>(SharedPaintBinder{data})};
    attributes.readDataDrivenPaintProperties<SharedPaintProperty>(
        binders, SharedPaintEvaluation{constant}, uniforms, paintAttributeID);
}

} // namespace

TEST(VertexAttribute, SharedPaintUpdatesReuseTheAttributeObject) {
    CountingAttributes attributes;
    const auto data = std::make_shared<FixedVertices>();

    for (unsigned update = 0; update < 128; ++update) {
        readPaint(attributes, data);
    }

    EXPECT_EQ(attributes.creations, 1u);
    ASSERT_TRUE(attributes.get(paintAttributeID));
    EXPECT_EQ(attributes.get(paintAttributeID)->getSharedRawData(), data);
    EXPECT_EQ(attributes.get(paintAttributeID)->getCount(), 4u);
}

TEST(VertexAttribute, ExplicitSetStillReplacesTheAttribute) {
    CountingAttributes attributes;
    attributes.set(paintAttributeID, 1, gfx::AttributeDataType::Float, 1)->set(0, 9.0f);

    const auto& replacement = attributes.set(paintAttributeID, 3, gfx::AttributeDataType::Float2, 2);

    ASSERT_TRUE(replacement);
    EXPECT_EQ(attributes.creations, 2u);
    EXPECT_EQ(replacement->getIndex(), 3);
    EXPECT_EQ(replacement->getDataType(), gfx::AttributeDataType::Float2);
    EXPECT_EQ(replacement->getCount(), 2u);
}

TEST(VertexAttribute, SwitchingToAnOlderSharedDataSourceRequiresUpload) {
    gfx::VertexAttribute attribute(-1, gfx::AttributeDataType::Invalid, 0);
    attribute.setSharedRawData(std::make_shared<FixedVertices>(5.0), 0, 0, 4, gfx::AttributeDataType::Float);
    attribute.setDirty(false);
    const auto replacement = std::make_shared<FixedVertices>(1.0);

    attribute.setSharedRawData(replacement, 0, 0, 4, gfx::AttributeDataType::Float);

    EXPECT_TRUE(attribute.isModifiedAfter(uploadedAt));
    EXPECT_EQ(attribute.getSharedRawData(), replacement);
}

TEST(VertexAttribute, ChangingSharedLayoutRequiresUpload) {
    const auto data = std::make_shared<FixedVertices>();
    gfx::VertexAttribute attribute(-1, gfx::AttributeDataType::Invalid, 0);
    attribute.setSharedRawData(data, 0, 0, 4, gfx::AttributeDataType::Float);
    attribute.setDirty(false);

    attribute.setSharedRawData(data, 4, 1, 8, gfx::AttributeDataType::Float2);

    EXPECT_TRUE(attribute.isModifiedAfter(uploadedAt));
    EXPECT_EQ(attribute.getSharedOffset(), 4u);
    EXPECT_EQ(attribute.getSharedVertexOffset(), 1u);
    EXPECT_EQ(attribute.getSharedStride(), 8u);
    EXPECT_EQ(attribute.getSharedType(), gfx::AttributeDataType::Float2);
}

TEST(VertexAttribute, UnchangedSharedDataDoesNotRequireUpload) {
    const auto data = std::make_shared<FixedVertices>();
    gfx::VertexAttribute attribute(-1, gfx::AttributeDataType::Invalid, 0);
    attribute.setSharedRawData(data, 0, 0, 4, gfx::AttributeDataType::Float);
    attribute.setDirty(false);
    const auto uploadTime = util::MonotonicTimer::now();

    attribute.setSharedRawData(data, 0, 0, 4, gfx::AttributeDataType::Float);

    EXPECT_FALSE(attribute.isModifiedAfter(uploadTime));
}

TEST(VertexAttribute, NewSharedContentsRequireUpload) {
    gfx::VertexAttribute attribute(-1, gfx::AttributeDataType::Invalid, 0);
    const auto data = std::make_shared<FixedVertices>();
    attribute.setSharedRawData(data, 0, 0, 4, gfx::AttributeDataType::Float);
    attribute.setDirty(false);
    const auto uploadTime = util::MonotonicTimer::now();
    data->setModificationTime(uploadTime + std::chrono::duration<double>(1.0));

    EXPECT_TRUE(attribute.isModifiedAfter(uploadTime));
}

TEST(VertexAttribute, SwitchingSharedDataRemainsModifiedAfterAnotherConsumerUploads) {
    gfx::VertexAttribute attribute(-1, gfx::AttributeDataType::Invalid, 0);
    attribute.setSharedRawData(std::make_shared<FixedVertices>(5.0), 0, 0, 4, gfx::AttributeDataType::Float);
    attribute.setDirty(false);
    const auto uploadTime = util::MonotonicTimer::now();

    attribute.setSharedRawData(std::make_shared<FixedVertices>(1.0), 0, 0, 4, gfx::AttributeDataType::Float);
    attribute.setDirty(false);

    EXPECT_TRUE(attribute.isModifiedAfter(uploadTime));
}

TEST(VertexAttribute, SwitchingPaintToConstantRemovesTheSharedAttribute) {
    CountingAttributes attributes;
    const auto data = std::make_shared<FixedVertices>();
    readPaint(attributes, data);
    gfx::StringIDSetsPair uniforms;

    readPaint(attributes, data, true, &uniforms);

    EXPECT_FALSE(attributes.get(paintAttributeID));
    EXPECT_EQ(uniforms.first.count("a_value"), 1u);
    EXPECT_EQ(uniforms.second.count(paintAttributeID), 1u);
}

TEST(VertexAttribute, SwitchingPaintBackToAttributeUsesTheNewSource) {
    CountingAttributes attributes;
    readPaint(attributes, std::make_shared<FixedVertices>(5.0));
    readPaint(attributes, std::make_shared<FixedVertices>(), true);
    const auto replacement = std::make_shared<FixedVertices>(1.0, 2);

    readPaint(attributes, replacement);

    ASSERT_TRUE(attributes.get(paintAttributeID));
    EXPECT_EQ(attributes.get(paintAttributeID)->getSharedRawData(), replacement);
    EXPECT_EQ(attributes.get(paintAttributeID)->getCount(), 2u);
    EXPECT_TRUE(attributes.get(paintAttributeID)->isModifiedAfter(uploadedAt));
}

TEST(VertexAttribute, EmptyPaintDataRemovesTheSharedAttribute) {
    CountingAttributes attributes;
    readPaint(attributes, std::make_shared<FixedVertices>());

    readPaint(attributes, std::make_shared<FixedVertices>(1.0, 0));

    EXPECT_FALSE(attributes.get(paintAttributeID));
}
