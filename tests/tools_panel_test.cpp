// The bundled tools-panel plugin: its rows from a TabActivitySnapshot, and the
// view over HostUi::Activity.

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

#include "ftxui_harness.hpp"
#include "tools_panel.hpp"

#include "minimal/client/sdk/theme.hpp"

namespace plugins = minimal::client::plugins;
using minimal::client::ColorDepth;
using minimal::client::Theme;
using minimal_test::RenderElement;
using minimal_test::RenderFrame;

namespace {

Theme TestTheme() { return Theme::Parse(R"({"name":"t","colors":{}})", ColorDepth::Truecolor); }

plugins::ToolActivity Tool(std::string name, bool running, bool failed = false) {
    plugins::ToolActivity tool;
    tool.name = std::move(name);
    tool.running = running;
    tool.failed = failed;
    tool.duration_ms = running ? -1 : 1500;
    return tool;
}

// A host that answers Activity with `snapshot`.
class ActivityUi final : public plugins::HostUi {
public:
    std::shared_ptr<const plugins::TabActivitySnapshot> snapshot =
        std::make_shared<const plugins::TabActivitySnapshot>();
    int redraws_after = 0;
    void RequestRedraw() override {}
    void RequestRedrawAfter(std::chrono::milliseconds) override { ++redraws_after; }
    void ShowView(const std::string&) override {}
    void CloseView(const std::string&) override {}
    void SetPanelVisible(const std::string&, bool) override {}
    void FocusPanel(const std::string&) override {}
    std::vector<plugins::KeyBindingInfo> KeyBindings() const override { return {}; }
    std::vector<minimal::client::CommandCatalogEntry> CommandCatalog() const override { return {}; }
    std::vector<std::string> ThemeNames() const override { return {}; }
    std::shared_ptr<const plugins::TabActivitySnapshot> Activity() const override { return snapshot; }
};

}  // namespace

TEST_CASE("tools panel: running, finished and failed rows, with the output's tail",
          "[tools_panel]") {
    plugins::TabActivitySnapshot snapshot;
    plugins::TabActivity tab;
    tab.display_name = "build";
    auto running = Tool("bash", true);
    running.summary = "make -j8";
    running.output = std::make_shared<const std::string>("one\ntwo\nthree\n");
    tab.tools = {running, Tool("read", false), Tool("edit", false, /*failed=*/true)};
    snapshot.tabs = {tab};
    const auto frame =
        RenderElement(minimal::tools_panel::RenderPanel(snapshot, /*output_lines=*/2, TestTheme()),
                      100, 20);
    CHECK(frame.find("bash") != std::string::npos);
    CHECK(frame.find("make -j8") != std::string::npos);
    CHECK(frame.find("build") != std::string::npos);
    CHECK(frame.find("one") == std::string::npos);   // past the 2-line tail
    CHECK(frame.find("two") != std::string::npos);
    CHECK(frame.find("three") != std::string::npos);
    CHECK(frame.find("1.5s") != std::string::npos);  // a finished one's duration
}

TEST_CASE("tools panel: the scrolling frame follows the newest tool", "[tools_panel]") {
    // The host wraps a panel in a yframe, which scrolls only to a focused
    // descendant: without one it stayed on the first tools forever.
    plugins::TabActivitySnapshot snapshot;
    plugins::TabActivity tab;
    tab.display_name = "s";
    for (int i = 0; i < 10; ++i) tab.tools.push_back(Tool("tool" + std::to_string(i), true));
    snapshot.tabs = {tab};
    const auto frame = RenderElement(
        minimal::tools_panel::RenderPanel(snapshot, 5, TestTheme()) | ftxui::yframe, 40, 6);
    CHECK(frame.find("tool9") != std::string::npos);
    CHECK(frame.find("tool0") == std::string::npos);
}

TEST_CASE("tools panel: no activity says so", "[tools_panel]") {
    const auto frame =
        RenderElement(minimal::tools_panel::RenderPanel({}, 5, TestTheme()), 60, 4);
    CHECK(frame.find("(no tool activity)") != std::string::npos);
}

TEST_CASE("tools panel: the tail of a partial last line counts as a line", "[tools_panel]") {
    CHECK(minimal::tools_panel::TailStart("a\nb\nc\nd", 2) == 4);   // "c\nd"
    CHECK(minimal::tools_panel::TailStart("a\nb\n", 1) == 2);        // "b\n"
    CHECK(minimal::tools_panel::TailStart("abc", 0) == 3);
}

TEST_CASE("tools panel view: rebuilds on a new snapshot, and keeps time while a tool runs",
          "[tools_panel]") {
    ActivityUi ui;
    minimal::tools_panel::ToolsPanelView view(&ui, 5);
    view.OnTheme(TestTheme());
    auto component = view.component();
    CHECK(RenderFrame(component, 60, 6).find("(no tool activity)") != std::string::npos);
    CHECK(ui.redraws_after == 0);

    auto next = std::make_shared<plugins::TabActivitySnapshot>();
    next->version = 1;
    plugins::TabActivity tab;
    tab.tools = {Tool("bash", true)};
    next->tabs = {tab};
    ui.snapshot = next;
    CHECK(RenderFrame(component, 60, 6).find("bash") != std::string::npos);
    CHECK(ui.redraws_after > 0);   // the elapsed time needs the next second
}
