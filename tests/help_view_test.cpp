// help-view (plugins/client/help-view): the key bindings and /-commands
// overlays, over the host's reads and the widget kit.

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <unordered_map>
#include <vector>

#include "ftxui_harness.hpp"
#include "help_view.hpp"
#include "minimal/client/keymap_help_model.hpp"
#include "widget_kit_fixture.hpp"

using namespace minimal;
using help_view::BuildCommandRows;
using help_view::BuildKeymapRows;

namespace {

// Bindings as the host hands them over: core's vocabulary on each.
std::vector<client::plugins::KeyBindingInfo> Bindings(
    const std::unordered_map<std::string, std::string>& keys) {
    std::vector<client::plugins::KeyBindingInfo> out;
    for (const auto& [id, key] : keys) {
        const auto info = client::DescribeKeyAction(id);
        out.push_back({id, key, info.description, info.group, info.order});
    }
    return out;
}

const std::unordered_map<std::string, std::string> kKeys = {
    {"tab.next", "alt+down"}, {"app.quit", "ctrl+q"}, {"agent.picker", "alt+g"}};

// The host, as far as the overlays see it.
class FakeHost final : public client::plugins::HostUi, public client::plugins::HostActions {
public:
    void RequestRedraw() override {}
    void ShowView(const std::string& id) override { shown.push_back(id); }
    void CloseView(const std::string& id) override { closed.push_back(id); }
    void SetPanelVisible(const std::string&, bool) override {}
    void FocusPanel(const std::string&) override {}
    std::vector<client::plugins::KeyBindingInfo> KeyBindings() const override { return Bindings(keys); }
    std::vector<client::CommandCatalogEntry> CommandCatalog() const override { return catalog; }
    std::vector<std::string> ThemeNames() const override { return {}; }
    void InsertPrompt(const std::string& text) override { inserted.push_back(text); }
    void SetTheme(const std::string&) override {}
    void ChooseAgent(const std::string&) override {}
    void ChooseModel(const std::string&) override {}
    void ChooseDefaultModel(const std::string&) override {}
    void SetSubagentModel(const std::string&) override {}
    void NewTab() override {}
    void OpenSession(const std::string&, bool) override {}
    void OpenSubagent(const std::string&, const std::string&, bool) override {}

    std::unordered_map<std::string, std::string> keys = kKeys;
    std::vector<client::CommandCatalogEntry> catalog;
    std::vector<std::string> shown, closed, inserted;
};

client::Theme TestTheme() {
    return client::Theme::Parse(R"({"name":"test","colors":{}})", client::ColorDepth::Truecolor);
}

}  // namespace

TEST_CASE("keymap rows: grouped by area, key column first, described in words", "[help_view]") {
    const auto rows = BuildKeymapRows(Bindings(kKeys), "");
    REQUIRE(rows.size() == 6);
    CHECK(rows[0].header);
    CHECK(rows[0].label == " tabs");
    CHECK(rows[1].id == "tab.next");
    CHECK(rows[1].label == "alt+down  next tab");
    CHECK(rows[2].label == " open");
    CHECK(rows[3].id == "agent.picker");
    CHECK(rows[3].label == "alt+g     switch agent");  // key column padded to the widest
    CHECK(rows[4].label == " app");
    CHECK(rows[5].id == "app.quit");
    CHECK_FALSE(rows[5].header);
}

TEST_CASE("keymap rows: within a group, core's curated order -- not the alphabet",
          "[help_view]") {
    const auto rows = BuildKeymapRows(
        Bindings({{"tab.goto.2", "alt+2"}, {"tab.close", "ctrl+w"}, {"tab.previous", "alt+up"},
                  {"tab.next", "alt+down"}, {"group.next", "alt+right"}}),
        "");
    REQUIRE(rows.size() == 6);
    CHECK(rows[1].id == "tab.next");
    CHECK(rows[2].id == "tab.previous");
    CHECK(rows[3].id == "tab.goto.2");
    CHECK(rows[4].id == "tab.close");
    CHECK(rows[5].id == "group.next");
}

TEST_CASE("keymap rows: the filter matches the id, the key or the description",
          "[help_view]") {
    SECTION("on the id and the description alike") {
        const auto rows = BuildKeymapRows(Bindings(kKeys), "TAB");
        REQUIRE(rows.size() == 2);  // the "tabs" header + tab.next
        CHECK(rows[1].id == "tab.next");
    }
    SECTION("on the key") {
        CHECK(BuildKeymapRows(Bindings(kKeys), "ALT").size() == 4);  // two groups, one each
    }
    SECTION("on the description only") {
        const auto rows = BuildKeymapRows(Bindings(kKeys), "switch");
        REQUIRE(rows.size() == 2);
        CHECK(rows[1].id == "agent.picker");
    }
    SECTION("no match: no rows, not even headers") {
        CHECK(BuildKeymapRows(Bindings(kKeys), "nonexistent").empty());
    }
}

TEST_CASE("keymap rows: a view's toggle is listed under views, last", "[help_view]") {
    const auto rows = BuildKeymapRows(Bindings({{"config", "ctrl+k"}, {"app.quit", "ctrl+q"}}), "");
    REQUIRE(rows.size() == 4);
    CHECK(rows[2].label == " views");
    CHECK(rows[3].label == "ctrl+k  config");
}

TEST_CASE("command rows: every entry, with what kind it is, filtered case-insensitively",
          "[help_view]") {
    std::vector<client::CommandCatalogEntry> entries;
    for (int i = 0; i < 15; ++i) {
        entries.push_back({.name = "cmd" + std::to_string(i), .summary = "summary"});
    }
    entries.push_back({.name = "review", .summary = "review the diff", .submits_prompt = true});
    entries.push_back({.name = "cost", .summary = "spend so far", .read_only = true});
    const auto rows = BuildCommandRows(entries, "");
    REQUIRE(rows.size() == 17);
    CHECK(rows[15].label == "/review  review the diff  (submits a prompt as your turn)");
    CHECK(rows[16].label == "/cost  spend so far  (read-only)");
    CHECK(rows[16].id == "cost");
    REQUIRE(BuildCommandRows(entries, "SPEND").size() == 1);
}

TEST_CASE("the commands overlay: Enter puts the command in the prompt and closes",
          "[help_view]") {
    FakeHost host;
    host.catalog = {{.name = "compact", .summary = "summarize"},
                    {.name = "clear", .summary = "start over"}};
    help_view::CommandsView view({&host, &host, &minimal_test::CoreWidgetKit()});
    view.OnTheme(TestTheme());
    auto component = view.component();
    view.OnShow();
    const std::string frame = minimal_test::RenderFrame(component, 100, 24);
    CHECK(frame.find("/compact") != std::string::npos);
    CHECK(frame.find("enter: insert") != std::string::npos);
    minimal_test::SendEvent(component, ftxui::Event::Return);
    CHECK(host.inserted == std::vector<std::string>{"/compact "});
    CHECK(host.closed == std::vector<std::string>{help_view::kCommandsViewId});

    // Each open reads the focused tab's catalog afresh.
    host.catalog = {{.name = "cost", .summary = "spend"}};
    view.OnShow();
    CHECK(minimal_test::RenderFrame(component, 100, 24).find("/cost") != std::string::npos);
    CHECK(minimal_test::RenderFrame(component, 100, 24).find("/compact") == std::string::npos);
}

TEST_CASE("the keymap overlay: the host's bindings; Enter or Escape closes it", "[help_view]") {
    FakeHost host;
    help_view::KeymapView view({&host, &host, &minimal_test::CoreWidgetKit()});
    view.OnTheme(TestTheme());
    CHECK(view.spec().id == std::string("help.overlay"));  // core's old action id
    CHECK(view.spec().default_keybinding == "alt+h");
    auto component = view.component();
    view.OnShow();
    CHECK(minimal_test::RenderFrame(component, 100, 24).find("switch agent") != std::string::npos);
    minimal_test::SendEvent(component, ftxui::Event::Return);
    minimal_test::SendEvent(component, ftxui::Event::Escape);
    CHECK(host.closed == std::vector<std::string>{"help.overlay", "help.overlay"});
}

TEST_CASE("/help opens the commands overlay only bare; /help <command> stays the server's",
          "[help_view]") {
    FakeHost host;
    help_view::HelpCommand help(&host);
    CHECK_FALSE(help.listed());  // the server lists /help
    CHECK_FALSE(help.Run({"compact"}, "compact"));
    CHECK(host.shown.empty());
    CHECK(help.Run({}, ""));
    help_view::KeymapCommand keymap(&host);
    CHECK(keymap.Run({"ignored"}, "ignored"));
    CHECK(host.shown == std::vector<std::string>{"help.commands", "help.overlay"});
}
