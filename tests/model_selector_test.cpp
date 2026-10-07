// The widget kit's model selector (view/model_selector.hpp): the model
// picker plus its own fetch.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "ftxui_harness.hpp"
#include "minimal/client/sdk/task_worker.hpp"
#include "model_selector.hpp"

using namespace minimal;
using namespace std::chrono_literals;

namespace {

client::Theme TestTheme() {
    return client::Theme::Parse(R"({"name":"test","colors":{}})", client::ColorDepth::Truecolor);
}

v1::ListModelsResponse Models(std::initializer_list<const char*> refs) {
    v1::ListModelsResponse response;
    for (const char* ref : refs) response.add_models()->set_ref(ref);
    return response;
}

// A model source the test controls: answers when released, records calls.
class Source {
public:
    std::optional<v1::ListModelsResponse> Fetch(bool refresh, std::string& error) {
        std::unique_lock<std::mutex> lock(mtx_);
        refreshes_.push_back(refresh);
        cv_.wait(lock, [this] { return released_; });
        if (fail_) {
            error = "the server is away";
            return std::nullopt;
        }
        return Models({"main/alpha", "main/beta", "other/gamma"});
    }
    void Release(bool fail = false) {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            released_ = true;
            fail_ = fail;
        }
        cv_.notify_all();
    }
    std::vector<bool> refreshes() {
        std::lock_guard<std::mutex> lock(mtx_);
        return refreshes_;
    }

private:
    std::mutex mtx_;
    std::condition_variable cv_;
    bool released_ = false;
    bool fail_ = false;
    std::vector<bool> refreshes_;
};

template <typename Pred>
bool Eventually(Pred pred) {
    for (int i = 0; i < 500; ++i) {
        if (pred()) return true;
        std::this_thread::sleep_for(10ms);
    }
    return pred();
}

// The caller's worker every selector fetches on: test-lived, like a view's.
void PostOn(client::ModelSelectorSpec& spec, client::plugins::TaskWorker& worker) {
    spec.post = [&worker](std::function<void()> task) { worker.Post(std::move(task)); };
}

client::ModelSelectorSpec SpecOver(Source& source, client::plugins::TaskWorker& worker) {
    client::ModelSelectorSpec spec;
    PostOn(spec, worker);
    spec.fetch = [&source](bool refresh, std::string& error) {
        return source.Fetch(refresh, error);
    };
    spec.leading_row_label = "off (untagged)";
    spec.leading_row_id = "<unset>";
    return spec;
}

}  // namespace

TEST_CASE("the model selector loads the list itself, then picks a model or the leading row",
          "[model_selector]") {
    Source source;
    client::plugins::TaskWorker worker("test");  // after `source`: joined first
    std::vector<std::string> picked;
    const auto theme = TestTheme();
    auto selector = client::ui_kit::MakeModelSelector(
        SpecOver(source, worker), {.on_select = [&](const std::string& ref) { picked.push_back(ref); }},
        theme);

    CHECK(minimal_test::RenderFrame(selector, 80, 24).find("loading") != std::string::npos);
    source.Release();
    REQUIRE(Eventually([&] {
        return minimal_test::RenderFrame(selector, 80, 24).find("gamma") != std::string::npos;
    }));
    const std::string frame = minimal_test::RenderFrame(selector, 80, 24);
    CHECK(frame.find("off (untagged)") != std::string::npos);
    CHECK(frame.find("alpha") != std::string::npos);

    minimal_test::SendEvent(selector, ftxui::Event::Return);  // the leading row
    REQUIRE(picked.size() == 1);
    CHECK(picked[0] == "<unset>");
    minimal_test::SendEvent(selector, ftxui::Event::ArrowDown);  // onto the "main" header...
    (void)minimal_test::RenderFrame(selector, 80, 24);  // ...which a frame walks past: alpha
    minimal_test::SendEvent(selector, ftxui::Event::Return);
    REQUIRE(picked.size() == 2);
    CHECK(picked[1] == "main/alpha");
}

TEST_CASE("the model selector shows why the list failed, and ctrl+r refreshes discovery",
          "[model_selector]") {
    Source source;
    client::plugins::TaskWorker worker("test");  // after `source`: joined first
    const auto theme = TestTheme();
    auto selector = client::ui_kit::MakeModelSelector(SpecOver(source, worker), {}, theme);
    source.Release(/*fail=*/true);
    REQUIRE(Eventually([&] {
        return minimal_test::RenderFrame(selector, 80, 24).find("the server is away") !=
               std::string::npos;
    }));
    minimal_test::SendEvent(selector, ftxui::Event::CtrlR);
    REQUIRE(Eventually([&] { return source.refreshes().size() == 2; }));
    CHECK(source.refreshes() == std::vector<bool>{false, true});
}

TEST_CASE("the model selector offers a typed ref the list does not have", "[model_selector]") {
    Source source;
    client::plugins::TaskWorker worker("test");  // after `source`: joined first
    source.Release();
    std::vector<std::string> picked;
    const auto theme = TestTheme();
    auto spec = SpecOver(source, worker);
    spec.allow_custom = true;
    auto selector = client::ui_kit::MakeModelSelector(
        spec, {.on_select = [&](const std::string& ref) { picked.push_back(ref); }}, theme);
    REQUIRE(Eventually([&] {
        return minimal_test::RenderFrame(selector, 80, 24).find("gamma") != std::string::npos;
    }));
    for (const char c : std::string("main/zeta")) {
        minimal_test::SendEvent(selector, ftxui::Event::Character(c));
    }
    CHECK(minimal_test::RenderFrame(selector, 80, 24).find("use 'main/zeta'") != std::string::npos);
    minimal_test::SendEvent(selector, ftxui::Event::ArrowDown);  // past the leading row
    minimal_test::SendEvent(selector, ftxui::Event::Return);
    REQUIRE(picked.size() == 1);
    CHECK(picked[0] == "main/zeta");

    // A listed ref typed in full is not offered twice.
    for (int i = 0; i < 4; ++i) minimal_test::SendEvent(selector, ftxui::Event::Backspace);
    for (const char c : std::string("beta")) {
        minimal_test::SendEvent(selector, ftxui::Event::Character(c));
    }
    CHECK(minimal_test::RenderFrame(selector, 80, 24).find("use '") == std::string::npos);
}

TEST_CASE("a model selector dropped while its fetch is out never waits on it",
          "[model_selector]") {
    // The UI-freeze shape: a discovery refresh (seconds) in flight when the
    // modal is dropped on the UI thread. The drop returns at once; the
    // fetch finishes on the caller's worker into state nobody shows.
    Source source;
    std::atomic<int> redraws{0};
    {
        client::plugins::TaskWorker worker("test");
        const auto theme = TestTheme();
        auto spec = SpecOver(source, worker);
        spec.redraw = [&] { ++redraws; };
        auto selector = client::ui_kit::MakeModelSelector(spec, {}, theme);
        REQUIRE(Eventually([&] { return source.refreshes().size() == 1; }));
        const auto before = std::chrono::steady_clock::now();
        selector.reset();  // the fetch is still blocked in Source::Fetch
        CHECK(std::chrono::steady_clock::now() - before < 200ms);
        source.Release();  // the worker's join (end of scope) then finishes
    }
    CHECK(redraws.load() == 1);
}

TEST_CASE("a model selector without a worker says so", "[model_selector]") {
    const auto theme = TestTheme();
    client::ModelSelectorSpec spec;
    spec.fetch = [](bool, std::string&) { return std::optional<v1::ListModelsResponse>(); };
    auto selector = client::ui_kit::MakeModelSelector(spec, {}, theme);
    CHECK(minimal_test::RenderFrame(selector, 80, 24).find("no worker") != std::string::npos);
}

TEST_CASE("a default row is offered only when the server names a default, and names it",
          "[model_selector]") {
    const auto theme = TestTheme();
    client::plugins::TaskWorker worker("test");
    for (const bool has_default : {true, false}) {
        INFO("has_default " << has_default);
        client::ModelSelectorSpec spec;
        spec.fetch = [has_default](bool, std::string&) {
            auto response = Models({"main/alpha", "main/beta"});
            if (has_default) response.set_default_model("main/beta");
            return std::optional<v1::ListModelsResponse>(response);
        };
        PostOn(spec, worker);
        spec.leading_row_id = "<default>";
        spec.leading_row_is_default = true;
        std::vector<std::string> picked;
        auto selector = client::ui_kit::MakeModelSelector(
            spec, {.on_select = [&](const std::string& ref) { picked.push_back(ref); }}, theme);
        REQUIRE(Eventually([&] {
            return minimal_test::RenderFrame(selector, 80, 24).find("beta") != std::string::npos;
        }));
        const std::string frame = minimal_test::RenderFrame(selector, 80, 24);
        CHECK((frame.find("default (main/beta)") != std::string::npos) == has_default);
        CHECK((frame.find("default (") != std::string::npos) == has_default);
        minimal_test::SendEvent(selector, ftxui::Event::Return);
        if (has_default) {
            CHECK(picked == std::vector<std::string>{"<default>"});
        }
    }
}

TEST_CASE("a model selector asked for names answers the provider's model name, typed names too",
          "[model_selector]") {
    const auto theme = TestTheme();
    client::plugins::TaskWorker worker("test");
    client::ModelSelectorSpec spec;
    PostOn(spec, worker);
    spec.fetch = [](bool, std::string&) {
        v1::ListModelsResponse response;
        auto* listed = response.add_models();
        listed->set_ref("gw/fast");
        listed->set_model_name("vendor-fast-2025");   // what the provider is sent
        response.add_models()->set_ref("gw/plain");     // an older server: no name
        return std::optional<v1::ListModelsResponse>(response);
    };
    spec.allow_custom = true;
    spec.choose_model_name = true;
    std::vector<std::string> picked;
    auto selector = client::ui_kit::MakeModelSelector(
        spec, {.on_select = [&](const std::string& name) { picked.push_back(name); }}, theme);
    REQUIRE(Eventually([&] {
        return minimal_test::RenderFrame(selector, 80, 24).find("plain") != std::string::npos;
    }));
    minimal_test::SendEvent(selector, ftxui::Event::Return);   // gw/fast
    minimal_test::SendEvent(selector, ftxui::Event::ArrowDown);
    (void)minimal_test::RenderFrame(selector, 80, 24);
    minimal_test::SendEvent(selector, ftxui::Event::Return);   // gw/plain
    for (const char c : std::string("my-model")) {
        minimal_test::SendEvent(selector, ftxui::Event::Character(c));
    }
    CHECK(minimal_test::RenderFrame(selector, 80, 24).find("use 'my-model'") != std::string::npos);
    minimal_test::SendEvent(selector, ftxui::Event::Return);
    CHECK(picked == std::vector<std::string>{"vendor-fast-2025", "plain", "my-model"});
}
