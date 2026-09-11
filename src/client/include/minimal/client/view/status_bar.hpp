#pragma once

// The status bar: a format string of segments ("{version} | {model} | ...")
// resolved from values the app supplies. Segment ids are the E5 registry's
// address space; E4 ships the core ids. An id nobody provides renders NOTHING
// -- and RunTui warns at startup, once, naming the id (the keybindings.json
// rule for names unknown at compile time).

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <ftxui/dom/elements.hpp>

#include "minimal/client/render/theme.hpp"
#include "minimal/client/status_format.hpp"

namespace minimal::client {

// The segment ids this build can fill. The startup check against a user's
// format string is done against this set.
bool IsKnownStatusSegment(std::string_view id);

// Warned ids: format segments no provider exists for. RunTui reports them once
// at startup rather than every frame.
std::vector<std::string> UnknownStatusSegments(const std::string& format);

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
// smaller, WRONG number) is the one thing the bar must never show.
//   1. `{session}` is capped at min(24, leftover budget) columns, truncated
//      with an ellipsis, or dropped when the rest alone already overflows.
//   2. Still too wide: the lowest-priority segment goes, with the separator
//      that joined it, until the bar fits. Version and the reference detail
//      (token_detail, session) go first; permission_mode -- the one
//      safety-relevant value -- goes last. Unknown (plugin) ids sit in the
//      middle.
std::vector<StatusBarPart> ResolveStatusBar(const std::vector<StatusSegment>& format,
                                            const std::map<std::string, std::string>& values,
                                            int max_width);

// "62k/200k · 31%" (window known) or "4.2k tok" (not). Compact on purpose --
// the exact count was seven digits of noise at a glance -- but never
// truncated: every figure shown is the real one, rounded.
std::string FormatTokenUsage(std::int64_t used, std::int64_t max);

// `emphasis` colors individual segment values by id (activity by state,
// tokens near the window, a risky permission mode); unlisted values use the
// body color, literals the muted one.
ftxui::Element RenderStatusBar(const std::vector<StatusSegment>& format,
                               const std::map<std::string, std::string>& values,
                               const Theme& theme, int max_width,
                               const std::map<std::string, ftxui::Color>& emphasis = {});

}  // namespace minimal::client
