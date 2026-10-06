// theme-view (plugins/client/theme-view): the theme picker and /theme, over
// the host's ThemeNames read and SetTheme action.

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "ftxui_harness.hpp"
#include "theme_view.hpp"
#include "widget_kit_fixture.hpp"

using namespace minimal;

namespace {

class FakeHost final : public client::plugins::HostUi, public client::plugins::HostActions {
public:
    void RequestRedraw() override {}
    void ShowView(const std::string& id) override { shown.push_back(id); }
    void CloseView(const std::string& id) override { closed.push_back(id); }
    void SetPanelVisible(const std::string&, bool) override {}
    void FocusPanel(const std::string&) override {}
    std::vector<client::plugins::KeyBindingInfo> KeyBindings() const override { return {}; }
    std::vector<client::CommandCatalogEntry> CommandCatalog() const override { return {}; }
    std::vector<std::string> ThemeNames() const override { return names; }
    void InsertPrompt(const std::string&) override {}
    void SetTheme(const std::string& name) override { switched.push_back(name); }

    std::vector<std::string> names = {"dark", "light", "solarized"};
    std::vector<std::string> shown, closed, switched;
};

client::Theme Named(const std::string& name) {
    return client::Theme::Parse(R"({"name":")" + name + R"(","colors":{}})",
                                client::ColorDepth::Truecolor);
}

}  // namespace

TEST_CASE("theme rows: the active one marked, filtered case-insensitively", "[theme_view]") {
    const auto rows = theme_view::BuildThemeRows({"dark", "light"}, "light", "");
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].label == "  dark");
    CHECK(rows[1].label == "● light");
    CHECK(rows[1].id == "light");  // the bare name: what SetTheme takes
    REQUIRE(theme_view::BuildThemeRows({"dark", "light"}, "", "LIG").size() == 1);
}

TEST_CASE("the theme picker switches through the host and closes", "[theme_view]") {
    FakeHost host;
    theme_view::ThemeView view({&host, &host, &minimal_test::CoreWidgetKit()});
    view.OnTheme(Named("dark"));
    auto component = view.component();
    view.OnShow();
    const std::string frame = minimal_test::RenderFrame(component, 80, 20);
    CHECK(frame.find("● dark") != std::string::npos);
    CHECK(frame.find("solarized") != std::string::npos);
    minimal_test::SendEvent(component, ftxui::Event::ArrowDown);
    minimal_test::SendEvent(component, ftxui::Event::Return);
    CHECK(host.switched == std::vector<std::string>{"light"});
    CHECK(host.closed == std::vector<std::string>{"theme"});
}

TEST_CASE("/theme: bare opens the picker, a name switches at once", "[theme_view]") {
    FakeHost host;
    theme_view::ThemeCommand command({&host, &host, &minimal_test::CoreWidgetKit()});
    CHECK(command.read_only());  // usable on a subagent tab too
    CHECK(command.Run({}, ""));
    CHECK(host.shown == std::vector<std::string>{"theme"});
    CHECK(command.Run({"light"}, "light"));
    CHECK(host.switched == std::vector<std::string>{"light"});
}
