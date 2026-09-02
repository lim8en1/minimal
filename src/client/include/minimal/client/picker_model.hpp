#pragma once

// Pure row-building logic for the session picker (T8), extracted the same
// way keybinding_dispatch.hpp pulled the tab-set math out of RunTui: index
// and filter math over a snapshot value type is exactly the class of bug
// that shipped unnoticed inside app.cpp before ("jump.attention" scanning
// from a fixed index instead of the focused tab). No FTXUI, no AppState, no
// network -- (sessions, filter, sort) -> rows, over the same
// v1::SessionSummary list app.cpp already fetches.

#include <string>
#include <vector>

#include "minimal/v1/session_service.pb.h"

namespace minimal::client {

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
std::vector<PickerRow> BuildPickerRows(const std::vector<v1::SessionSummary>& sessions,
                                       const std::string& filter, PickerSort sort);

}  // namespace minimal::client
