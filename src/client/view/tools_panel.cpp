#include "minimal/client/view/tools_panel.hpp"

#include <algorithm>
#include <chrono>
#include <string_view>

#include "minimal/client/chat_model.hpp"
#include "minimal/client/utf8.hpp"
#include "minimal/client/view/tool_status.hpp"

namespace minimal::client {

namespace {

// The session-name column's width, in display columns.
constexpr std::size_t kSessionColumn = 16;

// The starting BYTE OFFSET of the last `max_lines` lines of `text` -- an
// offset into the ORIGINAL buffer, not a copy of the tail. This function ran
// over every tool's WHOLE buffered output (up to tool_output_buffer_bytes_,
// 64 KiB default) on every render regardless of whether the panel was even
// showing new output, first via this backward scan and then via a
// text.substr() COPY of everything past it, which the caller then fed to an
// istringstream to split into rows -- three passes and at least two
// allocations of the tail's own size for content that render only ever reads
// once. Returning an offset lets the caller walk the original buffer with a
// string_view instead.
std::size_t TailStart(std::string_view text, int max_lines) {
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
    // The title only: the panel lists finished calls too, and "the transcript
    // is authoritative" was a note for developers, not for the reader.
    rows.push_back(text(" tools ") | bgcolor(theme.ColorFor("status_bar_bg")) | bold);

    bool any = false;
    for (const auto& tab : tabs) {
        for (const auto& tool : tab.tools) {
            any = true;
            Elements line;
            const std::string name = tool.tool_name.empty() ? std::string("tool") : tool.tool_name;
            const ToolStatus status = tool.started    ? ToolStatus::Running
                                      : tool.is_error ? ToolStatus::Failed
                                                      : ToolStatus::Ok;
            line.push_back(text(" "));
            line.push_back(ToolHeadline(status, name, theme));
            // The session as a fixed-width column, so the summaries line up
            // across tabs instead of starting wherever each name ended.
            std::string session = TruncateUtf8Columns(tab.display_name, kSessionColumn);
            session += std::string(
                kSessionColumn - std::min(kSessionColumn, Utf8ColumnWidth(session)), ' ');
            line.push_back(text("  " + session) | Muted(theme));
            // OneLineSummary guards BOTH paths -- a background bash row's
            // wire-carried command can be multi-line, and this panel's rows
            // are single text() lines like the live tool row's.
            const std::string summary =
                (tool.summary.has_value() && !tool.summary->empty())
                    ? OneLineSummary(*tool.summary)
                    : SummarizeToolInput(tool.tool_name, tool.input);
            if (!summary.empty()) {
                line.push_back(text("  " + summary) | Muted(theme));
            }
            // "started-at" (ARCHITECTURE.md's promised column) plus elapsed:
            // without a ticking number, a long-running cross-session command
            // that has gone quiet is indistinguishable from a hung one.
            if (tool.started_at_unix_ms.count() >= 0) {
                const std::string stamp = FormatClockTime(tool.started_at_unix_ms.count());
                if (!stamp.empty()) {
                    line.push_back(text("  " + stamp) | Muted(theme));
                }
            }
            if (tool.started) {
                if (tool.started_at_unix_ms.count() >= 0) {
                    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::system_clock::now().time_since_epoch())
                                            .count();
                    const auto elapsed_ms =
                        std::max<std::int64_t>(0, now_ms - tool.started_at_unix_ms.count());
                    line.push_back(text("  " + FormatDuration(elapsed_ms)) | Muted(theme));
                }
            } else if (tool.duration.count() >= 0) {
                line.push_back(text("  " + FormatDuration(tool.duration.count())) | Muted(theme));
            }
            rows.push_back(hbox(std::move(line)));
            // The published view (state.hpp): a shared, immutable string the
            // snapshot carries by pointer -- SessionState republishes it only
            // when the working buffer changed, so this render reads today's
            // output without any copy having been made for it. Null = the
            // snapshot carried no output (shouldn't happen for this caller,
            // which always passes include_tool_output=true) -- render no tail.
            const std::string_view buffered(
                tool.buffered_output ? std::string_view(*tool.buffered_output)
                                     : std::string_view{});
            const std::size_t tail_start = TailStart(buffered, stdout_buffer_lines);
            if (tail_start < buffered.size()) {
                // Indent the live stdout under its row. A view over the
                // ORIGINAL buffer (see TailStart) -- no substr copy of the
                // tail, no istringstream; one std::string allocation per
                // OUTPUT row, which is unavoidable (ftxui::text() needs one),
                // not per byte of the buffer scanned to find them.
                std::string_view tail(buffered);
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
                                   color(theme.ColorFor("system_message")));
                    if (newline == std::string_view::npos) break;
                    pos = newline + 1;
                }
            }
        }
    }
    if (!any) rows.push_back(text(" (no tool activity)") | Muted(theme));
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
