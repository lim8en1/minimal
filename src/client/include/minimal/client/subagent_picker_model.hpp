#pragma once

// Pure row-building logic for the /subagents picker, mirroring
// picker_model.hpp's split for the session picker: (children, include_ended,
// filter) -> rows, no FTXUI, no AppState, no network -- over the
// v1::SessionSummary list PickerController fetches once via
// ListSessions(include_subagents) filtered to one parent.

#include <string>
#include <vector>

#include "minimal/v1/session_service.pb.h"

namespace minimal::client {

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

}  // namespace minimal::client
