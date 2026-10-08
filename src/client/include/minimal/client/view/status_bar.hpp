#pragma once

// The status bar: the composed pieces (status_layout.hpp -- each segment
// placed by its plugin, ui.json overriding) resolved from the values the
// registered segments supply this frame. Core owns the layout and knows no
// segment by name -- every id, the bundled ones included (session-status),
// is a plugin's.

#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include <ftxui/dom/elements.hpp>

#include "minimal/client/sdk/theme.hpp"
#include "minimal/client/status_layout.hpp"

namespace minimal::client {

// The pseudo-segment "{>}": everything after it right-aligns. Not a value --
// it never renders text and never takes a separator.
inline constexpr std::string_view kStatusSplitId = ">";

// One resolved piece, in render order: a literal, a segment's value (with its
// id, for per-segment styling), or the left/right split point.
struct StatusBarPart {
    bool is_literal = false;
    std::string text;
    std::string segment_id;   // empty for literals and the split
    bool is_split = false;
};

// The status bar's width decision, pure and independently testable
// (ARCHITECTURE.md "Testing": render output itself is not unit-tested, the
// logic behind it is). Resolves `format` + `values` into the final ordered
// parts, given the terminal width available in display columns
// (`max_width <= 0` means "don't clamp", e.g. an unknown terminal size).
//
// Overflow is resolved by DROPPING whole segments, never by clipping one: a
// status value silently losing digits (a truncated token count reading as a
// smaller, WRONG number) is the one thing the bar must never show -- unless
// the segment says it may be cut:
//   1. A segment with a column cap (StatusSegment::max_columns, in
//      `layout.caps` -- a name, not a figure) is truncated to min(cap,
//      leftover budget) columns with an ellipsis, or dropped when the rest
//      alone already overflows.
//   2. Still too wide: the lowest-priority segment goes, with the separator
//      that joined it, until the bar fits. Each segment ranks by its own
//      StatusSegment::priority(), in `layout.priorities` (unlisted:
//      kPluginStatusSegmentPriority).
struct StatusLayout {
    std::map<std::string, int> priorities;
    std::map<std::string, std::size_t> caps;
};
std::vector<StatusBarPart> ResolveStatusBar(const std::vector<StatusSegment>& format,
                                            const std::map<std::string, std::string>& values,
                                            int max_width, const StatusLayout& layout = {});

// `emphasis` colors individual segment values by id (each segment's
// StatusValue::role, resolved); unlisted values use the body color, literals
// the muted one.
// The columns a bar resolves within on a terminal `terminal_width` wide: one
// column of padding at each edge, so values never touch the frame. The TUI
// and `minimal status-bar` both resolve at this, so the sample's drops are
// the TUI's.
inline int StatusBarContentWidth(int terminal_width) {
    return terminal_width > 2 ? terminal_width - 2 : terminal_width;
}

ftxui::Element RenderStatusBar(const std::vector<StatusSegment>& format,
                               const std::map<std::string, std::string>& values,
                               const Theme& theme, int max_width,
                               const std::map<std::string, ftxui::Color>& emphasis = {},
                               const StatusLayout& layout = {});

}  // namespace minimal::client
