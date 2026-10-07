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

client::ModelSelectorSpec SpecOver(Source& source) {
    client::ModelSelectorSpec spec;
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
    std::vector<std::string> picked;
    const auto theme = TestTheme();
    auto selector = client::ui_kit::MakeModelSelector(
        SpecOver(source), {.on_select = [&](const std::string& ref) { picked.push_back(ref); }},
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
    const auto theme = TestTheme();
    auto selector = client::ui_kit::MakeModelSelector(SpecOver(source), {}, theme);
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
    source.Release();
    std::vector<std::string> picked;
    const auto theme = TestTheme();
    auto spec = SpecOver(source);
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

TEST_CASE("a model selector closed while its fetch is out goes away cleanly",
          "[model_selector]") {
    Source source;
    const auto theme = TestTheme();
    std::atomic<int> redraws{0};
    auto spec = SpecOver(source);
    spec.redraw = [&] { ++redraws; };
    {
        auto selector = client::ui_kit::MakeModelSelector(spec, {}, theme);
        REQUIRE(Eventually([&] { return source.refreshes().size() == 1; }));
        std::thread release([&] {
            std::this_thread::sleep_for(20ms);
            source.Release();
        });
        selector.reset();  // waits for the fetch in flight, then joins
        release.join();
    }
    CHECK(redraws.load() <= 1);
}

TEST_CASE("a default row is offered only when the server names a default, and names it",
          "[model_selector]") {
    const auto theme = TestTheme();
    for (const bool has_default : {true, false}) {
        INFO("has_default " << has_default);
        client::ModelSelectorSpec spec;
        spec.fetch = [has_default](bool, std::string&) {
            auto response = Models({"main/alpha", "main/beta"});
            if (has_default) response.set_default_model("main/beta");
            return std::optional<v1::ListModelsResponse>(response);
        };
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
