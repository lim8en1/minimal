#pragma once

// The session and subagent pickers' rows: pure, (sessions, filter) -> rows,
// no FTXUI. Moved here with the pickers from client core.

#include <cstdint>
#include <string>
#include <vector>

#include "minimal/v1/session_service.pb.h"

namespace minimal::session_view {

namespace v1 = minimal::v1;

enum class PickerSort { kRecent, kName };

struct PickerRow {
    std::string label;        // display text
    std::string session_id;   // empty => the synthetic "+ new session" row
};

// Builds the picker's row list: a synthetic "+ new session" row first --
// ALWAYS, regardless of filter or sort, because the escape hatch to start
// fresh must never depend on what the user happened to type into the filter
// -- followed by every session whose name, id, or project_root contains
// `filter` case-insensitively, in `sort` order.
//
// `sessions` is the RAW accumulated list, in whatever order the caller
// fetched it (server delivery order, or with further pages appended by
// "load more"). Sorting here is a pure DISPLAY transform over a copy of
// that list and never reorders the caller's own vector -- which is what
// keeps "load more"'s cursor (derived from the raw list's last element)
// correct regardless of which sort the user currently has selected.
//
// Session labels are COLUMNS: a live marker ("● " for a session still running
// server-side), the name padded to the widest shown (capped), the message
// count right-aligned, then the age relative to `now_unix_ms` ("2h ago";
// a date past a week). 0 = the system clock -- tests pass a fixed now.
std::vector<PickerRow> BuildPickerRows(const std::vector<v1::SessionSummary>& sessions,
                                       const std::string& filter, PickerSort sort,
                                       std::int64_t now_unix_ms = 0);

// "just now" / "5m ago" / "3h ago" / "2d ago", then "YYYY-MM-DD" (local) past
// a week; "" for an unknown (<= 0) timestamp.
std::string RelativeAge(std::int64_t unix_ms, std::int64_t now_unix_ms);


struct SubagentPickerRow {
    std::string label;        // display text: "glyph name  ·  N msgs  ·  state  ·  age"
    std::string session_id;   // the child session the row opens
    bool is_live = false;     // a Session object still exists server-side
};

// The parent's children: LIVE rows first (the default view shows only these),
// then ended ones (only when `include_ended` -- the picker's ctrl+a). The
// server summary carries liveness only, so an ended row is labeled "ended"
// rather than pretending to distinguish errored from finished; an OPEN tab's
// own state is richer, but this model is deliberately tab-independent (the
// picker must be correct with the tab closed).
std::vector<SubagentPickerRow> BuildSubagentPickerRows(
    const std::vector<v1::SessionSummary>& sessions, bool include_ended,
    const std::string& filter, std::int64_t now_unix_ms = 0);   // 0: the real clock

}  // namespace minimal::session_view
