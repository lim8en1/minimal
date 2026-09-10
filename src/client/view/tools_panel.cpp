#include "minimal/client/view/tools_panel.hpp"

#include <string_view>

namespace minimal::client {

namespace {

// The starting BYTE OFFSET of the last `max_lines` lines of `text` -- an
// offset into the ORIGINAL string, not a copy of the tail. This function ran
// over every tool's WHOLE buffered_output (up to tool_output_buffer_bytes_,
// 64 KiB default) on every render regardless of whether the panel was even
// showing new output, first via this backward scan and then via a
// text.substr() COPY of everything past it, which the caller then fed to an
// istringstream to split into rows -- three passes and at least two
// allocations of the tail's own size for content that render only ever reads
// once. Returning an offset lets the caller walk the original buffer with a
// string_view instead.
std::size_t TailStart(const std::string& text, int max_lines) {
    if (max_lines <= 0) return text.size();
    int newlines = 0;
    for (std::size_t i = text.size(); i > 0; --i) {
        if (text[i - 1] == '\n') {
            ++newlines;
            if (newlines > max_lines) return i;
        }
    }
    return 0;
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
            const std::size_t tail_start = TailStart(tool.buffered_output, stdout_buffer_lines);
            if (tail_start < tool.buffered_output.size()) {
                // Indent the live stdout under its row. A view over the
                // ORIGINAL buffer (see TailStart) -- no substr copy of the
                // tail, no istringstream; one std::string allocation per
                // OUTPUT row, which is unavoidable (ftxui::text() needs one),
                // not per byte of the buffer scanned to find them.
                std::string_view tail(tool.buffered_output);
                tail.remove_prefix(tail_start);
                // `pos < tail.size()`, NOT `<=`: matching std::getline's own
                // behavior (which the istringstream version this replaces
                // relied on) is what this loop bound buys -- a trailing '\n'
                // ends the last line, it does not open a new EMPTY one.
                // `<=` would push one spurious blank row every time the live
                // buffer happened to end right after a newline, which is the
                // common case (most tool output is line-buffered).
                std::size_t pos = 0;
                while (pos < tail.size()) {
                    const std::size_t newline = tail.find('\n', pos);
                    const std::size_t end = newline == std::string_view::npos ? tail.size() : newline;
                    rows.push_back(text("   " + std::string(tail.substr(pos, end - pos))) |
                                   color(theme.ColorFor("system_message")) | dim);
                    if (newline == std::string_view::npos) break;
                    pos = newline + 1;
                }
            }
        }
    }
    if (!any) rows.push_back(text(" (no tool activity)") | dim);
    // Pin the view to the newest row: the caller wraps this in yframe
    // (frame_renderer.cpp), which only auto-scrolls to a descendant marked
    // via focus() -- without one it stays pinned at offset 0 forever (the
    // header and the first tool's earliest tail lines), so later tools and
    // newer output never show. rows is append-only, newest last, so the
    // last row is always the newest activity; no persisted scroll state is
    // needed, unlike ChatView's RowKey anchor.
    if (!rows.empty()) rows.back() = focus(std::move(rows.back()));
    return vbox(std::move(rows));
}

}  // namespace minimal::client
