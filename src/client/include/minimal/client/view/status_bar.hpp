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
//      safety-relevant value -- goes last. A plugin segment ranks by its own
//      StatusSegment::priority(), passed in `priorities` (default 30).
std::vector<StatusBarPart> ResolveStatusBar(const std::vector<StatusSegment>& format,
                                            const std::map<std::string, std::string>& values,
                                            int max_width,
                                            const std::map<std::string, int>& priorities = {});

// "62k/200k · 31%" (window known) or "4.2k tok" (not). Compact on purpose --
// the exact count was seven digits of noise at a glance -- but never
// truncated: every figure shown is the real one, rounded.
std::string FormatTokenUsage(std::int64_t used, std::int64_t max);

// The {permission_mode} segment's displayed value, pure and independently
// testable. A shift+tab sent but not yet confirmed by the server's metadata
// event is the tab's DESIRED state: while it differs from the applied mode -- and
// the tab has a session to apply into -- the desired value renders with the
// "→ " pending marker, the same desired-state convention {model}/{agent}
// follow. Empty `desired` (nothing pending) renders `applied`; both empty
// renders "" (the unknown-segment rule: the segment renders nothing).
std::string PermissionModeSegmentValue(const std::string& applied,
                                       const std::string& desired, bool has_session);

// The {activity} segment's composition with the live-view cut notice, pure
// and independently testable. When the live-segment count cap has dropped
// older segments of the current turn (SessionState's live_view_truncated
// latch), the suffix says so -- chrome about the session, riding the one
// segment that already means "what the session is doing right now".
// Appends with the " · " literal when `activity` is non-empty (the same
// separator spelling the bar renders between segments); a non-empty notice
// over an empty activity stands alone. Composed, never overriding: the
// state word (working / cancelling… / …) must survive the suffix.
std::string WithHiddenRowsSuffix(std::string activity);

// `emphasis` colors individual segment values by id (activity by state,
// tokens near the window, a risky permission mode); unlisted values use the
// body color, literals the muted one.
ftxui::Element RenderStatusBar(const std::vector<StatusSegment>& format,
                               const std::map<std::string, std::string>& values,
                               const Theme& theme, int max_width,
                               const std::map<std::string, ftxui::Color>& emphasis = {},
                               const std::map<std::string, int>& priorities = {});

}  // namespace minimal::client
