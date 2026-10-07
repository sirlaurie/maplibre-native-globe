#include <mln/layout/symbol_instance.hpp>
#include <mln/test/util.hpp>
#include <mln/util/error_sink.hpp>
#include <mln/util/symbol_error_observer.hpp>

#include <cstring>

using namespace mln;

#if MLN_SYMBOL_GUARDS
namespace {

SymbolInstance makeGuardedSymbol(std::u16string key = u"symbol") {
    const ShapedTextOrientations shaping{};
    const style::SymbolLayoutProperties::Evaluated layout;
    const IndexedSubfeature feature(0, {}, {}, 0);
    Anchor anchor(0, 0, 0, 0);
    const std::array<float, 2> offset{0, 0};
    auto data = std::make_shared<SymbolInstanceSharedData>(GeometryCoordinates{},
                                                           shaping,
                                                           std::nullopt,
                                                           std::nullopt,
                                                           layout,
                                                           style::SymbolPlacementType::Point,
                                                           offset,
                                                           ImageMap{},
                                                           0,
                                                           SymbolContent::None,
                                                           false,
                                                           false);
    return SymbolInstance(anchor,
                          std::move(data),
                          shaping,
                          std::nullopt,
                          std::nullopt,
                          0,
                          0,
                          style::SymbolPlacementType::Point,
                          offset,
                          0,
                          0,
                          offset,
                          feature,
                          0,
                          0,
                          std::move(key),
                          1,
                          0,
                          0,
                          std::nullopt,
                          false);
}

struct DetectedSymbolError {};

class RecordingSymbolErrorObserver final : public SymbolErrorObserver {
public:
    void onSymbolError(const std::string& message) override {
        errors.push_back(message);
        throw DetectedSymbolError{};
    }

    std::vector<std::string> errors;
};

class GuardCorruptionProbe final : public SymbolInstance {
public:
    explicit GuardCorruptionProbe(SymbolInstance symbol)
        : SymbolInstance(std::move(symbol)) {}

    using SymbolInstance::forceFailInternal;
};

std::vector<std::size_t> guardOffsets() {
    constexpr std::uint64_t guard = 0x123456780ABCDEFFULL;
    GuardCorruptionProbe symbol(makeGuardedSymbol());
    std::array<unsigned char, sizeof(SymbolInstance)> original;
    std::memcpy(original.data(), &symbol, original.size());
    symbol.forceFailInternal();
    const auto* bytes = reinterpret_cast<const unsigned char*>(&symbol);
    std::vector<std::size_t> offsets;
    for (std::size_t offset = 0; offset + sizeof(guard) <= original.size(); ++offset) {
        std::uint64_t before, after;
        std::memcpy(&before, original.data() + offset, sizeof(before));
        std::memcpy(&after, bytes + offset, sizeof(after));
        if (before == guard && after == 0) offsets.push_back(offset);
    }
    return offsets;
}

} // namespace

TEST(SymbolInstance, ValidGuardsSurviveRepeatedChecksAndCrossTileUpdates) {
    auto symbol = makeGuardedSymbol();
    for (std::uint32_t id = 1; id <= 100; ++id) {
        symbol.setCrossTileID(id);
        EXPECT_TRUE(symbol.check(SYM_GUARD_LOC));
    }
}

TEST(SymbolInstance, EveryCorruptedGuardIsReportedAndRemainsRejectedAfterRepair) {
    const auto offsets = guardOffsets();
    ASSERT_EQ(offsets.size(), 28u);
    for (std::size_t index = 0; index < offsets.size(); ++index) {
        SCOPED_TRACE(index + 1);
        auto symbol = makeGuardedSymbol();
        RecordingSymbolErrorObserver observer;
        ErrorScope scope(&observer);
        auto* bytes = reinterpret_cast<unsigned char*>(&symbol);
        bytes[offsets[index]] ^= 0x01;

        EXPECT_THROW(symbol.check(SYM_GUARD_LOC), DetectedSymbolError);
        ASSERT_EQ(observer.errors.size(), 1u);
        EXPECT_NE(observer.errors.front().find("SymbolInstance corrupted at " + std::to_string(index + 1) + " "),
                  std::string::npos);
        EXPECT_NE(observer.errors.front().find("symbol_instance.test.cpp:"), std::string::npos);
        bytes[offsets[index]] ^= 0x01;

        EXPECT_FALSE(symbol.check(SYM_GUARD_LOC));
        EXPECT_EQ(observer.errors.size(), 1u);
    }
}

TEST(SymbolInstance, KeyAtLengthLimitPasses) {
    const auto symbol = makeGuardedSymbol(std::u16string(10000, u'a'));
    EXPECT_TRUE(symbol.check(SYM_GUARD_LOC));
}

TEST(SymbolInstance, OversizedKeyIsReportedAndRejected) {
    auto symbol = makeGuardedSymbol(std::u16string(10001, u'a'));
    RecordingSymbolErrorObserver observer;
    ErrorScope scope(&observer);
    EXPECT_THROW(symbol.check(SYM_GUARD_LOC), DetectedSymbolError);
    ASSERT_EQ(observer.errors.size(), 1u);
    EXPECT_NE(observer.errors.front().find("key corrupted with size=10001"), std::string::npos);
    EXPECT_FALSE(symbol.check(SYM_GUARD_LOC));
}

TEST(SymbolInstance, AbsentIndexesPassWithEmptyBuffers) {
    const auto symbol = makeGuardedSymbol();
    EXPECT_TRUE(symbol.checkIndexes(0, 0, 0, SYM_GUARD_LOC));
}

TEST(SymbolInstance, LastTextIndexPasses) {
    auto symbol = makeGuardedSymbol();
    symbol.setPlacedRightTextIndex(2);
    EXPECT_TRUE(symbol.checkIndexes(3, 0, 0, SYM_GUARD_LOC));
}

TEST(SymbolInstance, OutOfBoundsTextIndexIsReportedAndRejected) {
    auto symbol = makeGuardedSymbol();
    symbol.setPlacedRightTextIndex(3);
    RecordingSymbolErrorObserver observer;
    ErrorScope scope(&observer);
    EXPECT_THROW(symbol.checkIndexes(3, 0, 0, SYM_GUARD_LOC), DetectedSymbolError);
    ASSERT_EQ(observer.errors.size(), 1u);
    EXPECT_NE(observer.errors.front().find("index corrupted with value=3 size=3"), std::string::npos);
    EXPECT_FALSE(symbol.check(SYM_GUARD_LOC));
}

TEST(SymbolInstance, IconIndexUsesIconBufferSize) {
    auto symbol = makeGuardedSymbol();
    symbol.refPlacedIconIndex() = 1;
    RecordingSymbolErrorObserver observer;
    ErrorScope scope(&observer);
    EXPECT_THROW(symbol.checkIndexes(0, 1, 10, SYM_GUARD_LOC), DetectedSymbolError);
    ASSERT_EQ(observer.errors.size(), 1u);
    EXPECT_FALSE(symbol.check(SYM_GUARD_LOC));
}

TEST(SymbolInstance, ExternalFailureRemainsRejected) {
    auto symbol = makeGuardedSymbol();
    RecordingSymbolErrorObserver observer;
    ErrorScope scope(&observer);
    EXPECT_THROW(symbol.forceFail(), DetectedSymbolError);
    EXPECT_FALSE(symbol.check(SYM_GUARD_LOC));
    EXPECT_FALSE(symbol.checkIndexes(0, 0, 0, SYM_GUARD_LOC));
    EXPECT_NO_THROW(symbol.forceFail());
    EXPECT_EQ(observer.errors.size(), 1u);
}
#endif
