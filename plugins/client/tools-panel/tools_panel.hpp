#pragma once

// tools-panel -- every tab's tool calls in one panel, with their live output:
// a cross-session monitor ("is this progressing?"), lossy by design; each
// session's transcript is the record. Reads HostUi::Activity.

#include <cstdint>
#include <memory>
#include <string_view>

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

#include "minimal/client/sdk/registries.hpp"
#include "minimal/client/sdk/theme.hpp"

namespace minimal::tools_panel {

namespace client = minimal::client;
namespace plugins = minimal::client::plugins;

inline constexpr const char* kViewId = "tools.panel";

// The panel's rows for `snapshot`: per tool its status, name, session,
// summary, start time and timing, then the last `output_lines` lines of its
// output. The last row carries focus, so the host's scrolling frame follows
// the newest activity.
ftxui::Element RenderPanel(const plugins::TabActivitySnapshot& snapshot, int output_lines,
                           const client::Theme& theme);

// The start of the last `max_lines` lines of `text` (a trailing partial line
// counts as one) -- an offset into it, so the tail is never copied.
std::size_t TailStart(std::string_view text, int max_lines);

class ToolsPanelView final : public plugins::View {
public:
    ToolsPanelView(plugins::HostUi* ui, int output_lines);
    const plugins::ViewSpec& spec() const override;
    ftxui::Component component() override;
    void OnTheme(const client::Theme& theme) override { theme_ = theme; }

private:
    plugins::HostUi* ui_;
    int output_lines_;
    client::Theme theme_;
};

}  // namespace minimal::tools_panel
