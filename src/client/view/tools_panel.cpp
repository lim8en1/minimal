#include "minimal/client/view/tools_panel.hpp"

#include <sstream>

namespace minimal::client {

namespace {

std::string TailLines(const std::string& text, int max_lines) {
    if (max_lines <= 0) return {};
    // Count from the back; keep the LAST max_lines lines.
    int newlines = 0;
    std::size_t start = 0;
    for (std::size_t i = text.size(); i > 0; --i) {
        if (text[i - 1] == '\n') {
            ++newlines;
            if (newlines > max_lines) {
                start = i;
                break;
            }
        }
    }
    return text.substr(start);
}

}  // namespace

ftxui::Element RenderToolsPanel(const std::vector<AppState::TabSnapshot>& tabs,
                                int stdout_buffer_lines, const Theme& theme) {
    using namespace ftxui;

    // Track R (focused-tab pump redesign): the panel reads the LIGHT
    // TabSnapshot.tools -- it needs every session's tool rows (cross-session
    // is what distinguishes it from chat), never other tabs' scrollback or
    // streaming text, so the frame no longer deep-copies every tab's full
    // snapshot to feed it.
    Elements rows;
    rows.push_back(hbox({text(" running tools ") | bgcolor(theme.ColorFor("status_bar_bg")),
                         text(" (live tail only; the transcript is authoritative)") | dim}));

    bool any = false;
    for (const auto& tab : tabs) {
        for (const auto& tool : tab.tools) {
            any = true;
            Elements line;
            const std::string name = tool.tool_name.empty() ? std::string("tool") : tool.tool_name;
            line.push_back(text(" " + name) | color(theme.ColorFor("syntax_function")));
            if (tool.started) {
                line.push_back(text(" running") | color(theme.ColorFor("assistant_message")));
            } else if (tool.is_error) {
                line.push_back(text(" FAILED") | color(theme.ColorFor("error")));
            } else {
                line.push_back(text(" done") | color(theme.ColorFor("system_message")));
            }
            line.push_back(text(" | " + tab.display_name) | dim);
            if (tool.summary.has_value() && !tool.summary->empty()) {
                line.push_back(text(" | " + *tool.summary) | dim);
            }
            rows.push_back(hbox(std::move(line)));
            const std::string tail = TailLines(tool.buffered_output, stdout_buffer_lines);
            if (!tail.empty()) {
                // Indent the live stdout under its row.
                std::istringstream stream(tail);
                std::string out_line;
                while (std::getline(stream, out_line)) {
                    rows.push_back(text("   " + out_line) | color(theme.ColorFor("system_message")) |
                                   dim);
                }
            }
        }
    }
    if (!any) rows.push_back(text(" (no tool activity)") | dim);
    return vbox(std::move(rows));
}

}  // namespace minimal::client
