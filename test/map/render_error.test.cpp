#include <mln/test/util.hpp>
#include <mln/test/map_adapter.hpp>
#include <mln/test/stub_file_source.hpp>
#include <mln/map/map_options.hpp>
#include <mln/renderer/renderer_frontend.hpp>
#include <mln/renderer/renderer_observer.hpp>
#include <mln/renderer/update_parameters.hpp>
#include <mln/style/style.hpp>
#include <mln/util/run_loop.hpp>

using namespace mln;

namespace {

class ErrorFrontend final : public RendererFrontend {
public:
    void reset() override { observer = nullptr; }
    void setObserver(RendererObserver& value) override { observer = &value; }
    void update(std::shared_ptr<UpdateParameters>) override {}
    const TaggedScheduler& getThreadPool() const override { return scheduler; }

    RendererObserver* observer = nullptr;

private:
    const TaggedScheduler scheduler{Scheduler::GetBackground(), util::SimpleIdentity::Empty};
};

class RenderErrorObserver final : public MapObserver {
public:
    void onRenderError(std::exception_ptr error) override { errors.push_back(error); }
    std::vector<std::exception_ptr> errors;
};

struct RenderErrorFixture {
    util::RunLoop loop;
    std::shared_ptr<StubFileSource> fileSource = std::make_shared<StubFileSource>();
    ErrorFrontend frontend;
    RenderErrorObserver observer;
    MapAdapter map{frontend, observer, fileSource, MapOptions().withMapMode(MapMode::Static).withSize({64, 64})};

    RenderErrorFixture() { map.getStyle().loadJSON(R"({"version":8,"sources":{},"layers":[]})"); }
};

} // namespace

TEST(MapRenderError, CompletesPendingStillRequestWithTheRenderError) {
    RenderErrorFixture fixture;
    const auto expected = std::make_exception_ptr(std::runtime_error("shader compilation failed"));
    std::vector<std::exception_ptr> results;
    fixture.map.renderStill([&](std::exception_ptr error) { results.push_back(error); });
    ASSERT_NE(fixture.frontend.observer, nullptr);
    fixture.frontend.observer->onRenderError(expected);

    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results.front(), expected);
    ASSERT_EQ(fixture.observer.errors.size(), 1u);
    EXPECT_EQ(fixture.observer.errors.front(), expected);
}

TEST(MapRenderError, FailedRequestCanBeReplacedWithAnotherStillRequest) {
    RenderErrorFixture fixture;
    const auto first = std::make_exception_ptr(std::runtime_error("first shader error"));
    const auto second = std::make_exception_ptr(std::runtime_error("second shader error"));
    std::vector<std::exception_ptr> results;
    fixture.map.renderStill([&](std::exception_ptr error) { results.push_back(error); });
    ASSERT_NE(fixture.frontend.observer, nullptr);
    fixture.frontend.observer->onRenderError(first);
    fixture.map.renderStill([&](std::exception_ptr error) { results.push_back(error); });
    fixture.frontend.observer->onRenderError(second);

    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0], first);
    EXPECT_EQ(results[1], second);
}

TEST(MapRenderError, LaterFullFrameDoesNotCompleteTheFailedRequestAgain) {
    RenderErrorFixture fixture;
    const auto expected = std::make_exception_ptr(std::runtime_error("shader compilation failed"));
    std::vector<std::exception_ptr> results;
    fixture.map.renderStill([&](std::exception_ptr error) { results.push_back(error); });
    ASSERT_NE(fixture.frontend.observer, nullptr);
    fixture.frontend.observer->onRenderError(expected);
    fixture.frontend.observer->onDidFinishRenderingFrame(
        RendererObserver::RenderMode::Full, false, false, std::make_shared<gfx::RenderingStats>());

    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results.front(), expected);
}
