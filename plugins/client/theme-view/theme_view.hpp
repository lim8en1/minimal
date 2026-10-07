#pragma once

// theme-view -- switching the UI theme: the "theme" modal (the themes there
// are, the active one marked) and /theme (bare opens it, /theme <name>
// switches at once). The switch itself is the host's (HostActions::SetTheme:
// it reloads the palette, re-pushes it to every view and remembers it).

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <ftxui/component/component.hpp>

#include "minimal/client/sdk/host.hpp"
#include "minimal/client/sdk/views.hpp"
#include "minimal/client/sdk/widgets.hpp"
#include "minimal/client/sdk/theme.hpp"

namespace minimal::theme_view {

namespace client = minimal::client;
namespace plugins = minimal::client::plugins;

inline constexpr const char* kViewId = "theme";

// The picker's rows: the names containing `filter` (case-insensitively), the
// active one marked -- a switch shows what it switches from.
std::vector<client::MenuRow> BuildThemeRows(const std::vector<std::string>& names,
                                            const std::string& current,
                                            const std::string& filter);

// The host seams the view and the command use (non-null; host-owned).
struct Seams {
    plugins::HostUi* ui;
    plugins::HostActions* actions;
    const plugins::WidgetKit* widgets;
};

class ThemeView final : public plugins::View {
public:
    explicit ThemeView(Seams seams) : seams_(seams) {}
    const plugins::ViewSpec& spec() const override;
    ftxui::Component component() override;
    // The active theme's name comes with it.
    void OnTheme(const client::Theme& theme) override { theme_ = theme; }
    void OnShow() const override { stale_->store(true); }  // the themes dir, read again

private:
    Seams seams_;
    client::Theme theme_;
    std::shared_ptr<std::atomic<bool>> stale_ = std::make_shared<std::atomic<bool>>(true);
};

// `/theme`: bare opens the picker; `/theme <name>` switches directly.
class ThemeCommand final : public plugins::ClientCommand {
public:
    explicit ThemeCommand(Seams seams) : seams_(seams) {}
    std::string_view name() const override { return "theme"; }
    std::string_view summary() const override { return "switch the UI theme"; }
    bool read_only() const override { return true; }  // decoration: any tab
    bool Run(const std::vector<std::string>& positional, const std::string&) override;

private:
    Seams seams_;
};

}  // namespace minimal::theme_view
