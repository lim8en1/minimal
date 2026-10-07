// selection-view (plugins/client/selection-view): the agent, model and
// subagent-model choosers core opens by role; every pick is a HostActions
// verb.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "ftxui_harness.hpp"
#include "selection_view.hpp"
#include "widget_kit_fixture.hpp"

using namespace minimal;
using namespace std::chrono_literals;

namespace {

v1::AgentPresetSummary Preset(std::string name, std::string description, std::string color = {}) {
    v1::AgentPresetSummary preset;
    preset.set_name(std::move(name));
    preset.set_description(std::move(description));
    preset.set_color(std::move(color));
    return preset;
}

// The host and the server, as far as the choosers see them. Called from the
// views' workers too, so what they read is set before the view opens.
class FakeHost final : public client::plugins::HostUi,
                       public client::plugins::HostActions,
                       public client::plugins::ClientApi {
public:
    void RequestRedraw() override {}
    void ShowView(const std::string&) override {}
    void CloseView(const std::string& id) override { closed.push_back(id); }
    void SetPanelVisible(const std::string&, bool) override {}
    void FocusPanel(const std::string&) override {}
    std::vector<client::plugins::KeyBindingInfo> KeyBindings() const override { return {}; }
    std::vector<client::CommandCatalogEntry> CommandCatalog() const override { return {}; }
    std::vector<std::string> ThemeNames() const override { return {}; }

    void InsertPrompt(const std::string&) override {}
    void SetTheme(const std::string&) override {}
    void ChooseAgent(const std::string& name) override { calls.push_back("agent " + name); }
    void ChooseModel(const std::string& ref) override { calls.push_back("model " + ref); }
    void ChooseDefaultModel(const std::string& ref) override { calls.push_back("default " + ref); }
    void SetSubagentModel(const std::string& ref) override { calls.push_back("subagent " + ref); }
    void NewTab() override {}
    void OpenSession(const std::string&, bool) override {}
    void OpenSubagent(const std::string&, const std::string&, bool) override {}
    void SetClientPluginEnabled(const std::string&, bool) override {}

    std::optional<MessagePage> GetMessages(const std::string&, std::optional<std::int64_t>,
                                           int) override {
        return std::nullopt;
    }
    std::optional<std::string> PluginCall(const std::string&, const std::string&,
                                          const std::string&) override {
        return std::nullopt;
    }
    std::optional<v1::ListModelsResponse> ListModels(bool, std::string*) override {
        v1::ListModelsResponse response;
        response.add_models()->set_ref("main/alpha");
        response.set_default_model("main/alpha");
        return response;
    }
    std::optional<v1::ListAgentPresetsResponse> ListAgentPresets(std::string*) override {
        v1::ListAgentPresetsResponse response;
        *response.add_presets() = Preset("build", "Builds things.");
        *response.add_presets() = Preset("plan", "Plans work.");
        return response;
    }
    std::optional<minimal::v1::ListSessionsResponse> ListSessions(const SessionQuery&,
                                                                  std::string*) override {
        return std::nullopt;
    }
    std::optional<std::vector<minimal::v1::SessionSummary>> ListSubagents(const std::string&,
                                                                          std::string*) override {
        return std::nullopt;
    }
    bool DeleteSession(const std::string&, std::string*) override { return false; }
    client::SubscribeOutcome PluginSubscribe(const std::string&, const std::string&,
                                             const std::string&,
                                             const std::function<bool(const std::string&)>&) override {
        return client::SubscribeOutcome::kError;
    }

    selection_view::Seams seams() { return {this, this, &minimal_test::CoreWidgetKit(), this}; }
    std::vector<std::string> calls, closed;
};

client::Theme TestTheme() {
    return client::Theme::Parse(R"({"name":"test","colors":{}})", client::ColorDepth::Truecolor);
}

template <typename Pred>
bool Eventually(Pred pred) {
    for (int i = 0; i < 500; ++i) {
        if (pred()) return true;
        std::this_thread::sleep_for(10ms);
    }
    return pred();
}

}  // namespace

TEST_CASE("agent rows: name and description, filtered on either, in the preset's color",
          "[selection_view]") {
    const std::vector<v1::AgentPresetSummary> presets = {
        Preset("build", "Runs the build.", "#ff0000"), Preset("plan", "")};
    const auto rows = selection_view::BuildAgentRows(presets, "");
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].label == "build  ·  Runs the build.");
    CHECK(rows[0].color == "#ff0000");
    CHECK(rows[1].label == "plan");  // no description, no separator
    REQUIRE(selection_view::BuildAgentRows(presets, "BUI").size() == 1);
    REQUIRE(selection_view::BuildAgentRows(presets, "runs").size() == 1);
    CHECK(selection_view::BuildAgentRows(presets, "zzz").empty());
}

TEST_CASE("the agents view lists the project's presets and chooses through the host",
          "[selection_view]") {
    FakeHost host;
    selection_view::AgentsView view(host.seams());
    view.OnTheme(TestTheme());
    auto component = view.component();
    view.OnShow();
    REQUIRE(Eventually([&] {
        return minimal_test::RenderFrame(component, 80, 20).find("plan") != std::string::npos;
    }));
    minimal_test::SendEvent(component, ftxui::Event::ArrowDown);
    minimal_test::SendEvent(component, ftxui::Event::Return);
    CHECK(host.calls == std::vector<std::string>{"agent plan"});
    CHECK(host.closed == std::vector<std::string>{"agents"});
}

TEST_CASE("the models view: a model, or back to the server's default", "[selection_view]") {
    FakeHost host;
    selection_view::ModelsView view(host.seams(), /*for_subagents=*/false);
    view.OnTheme(TestTheme());
    CHECK(view.spec().id == std::string("models"));  // the server's view_hint name
    auto component = view.component();
    view.OnShow();
    REQUIRE(Eventually([&] {
        return minimal_test::RenderFrame(component, 80, 20).find("alpha") != std::string::npos;
    }));
    minimal_test::SendEvent(component, ftxui::Event::Return);  // the default row
    minimal_test::SendEvent(component, ftxui::Event::ArrowDown);  // onto the "main" header...
    (void)minimal_test::RenderFrame(component, 80, 20);           // ...a frame walks past it
    minimal_test::SendEvent(component, ftxui::Event::Return);     // main/alpha
    CHECK(host.calls == std::vector<std::string>{"default main/alpha", "model main/alpha"});
}

TEST_CASE("the subagent-models view sets the session's subagent model, or the parent's",
          "[selection_view]") {
    FakeHost host;
    selection_view::ModelsView view(host.seams(), /*for_subagents=*/true);
    view.OnTheme(TestTheme());
    CHECK(view.spec().id == std::string("subagent-models"));
    auto component = view.component();
    view.OnShow();
    REQUIRE(Eventually([&] {
        return minimal_test::RenderFrame(component, 80, 20).find("same as parent") !=
               std::string::npos;
    }));
    minimal_test::SendEvent(component, ftxui::Event::Return);
    CHECK(host.calls == std::vector<std::string>{"subagent inherit"});
    CHECK(host.closed == std::vector<std::string>{"subagent-models"});
}
