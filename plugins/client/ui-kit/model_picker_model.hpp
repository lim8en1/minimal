#pragma once

// Pure row-building logic for the model picker, extracted from RunTui the
// same way picker_model.hpp pulled the session picker's filter math out:
// (models, filter) -> rows over a snapshot value type, no FTXUI, no
// AppState, no network. The 2-tier grouping rules are exactly the kind of
// index/filter logic that is worth unit-testing on its own.

#include <string>
#include <vector>

#include "minimal/v1/session_service.pb.h"

namespace minimal::client::ui_kit {

struct ModelPickerRow {
    std::string label;
    std::string ref;      // "<provider>/<model>" -- empty => provider header row
    bool header = false;  // true => bold non-selectable provider header
};

// Builds the picker's row list grouped by provider (2-tier): for each
// provider, a header row, then one row per model. The input is sorted by
// ref here (a ref sort groups by provider first), so the layout is correct
// for any input order -- the server's ascending delivery just happens to
// already be that order.
//
// An empty filter lists every model. A non-empty filter matches against the
// FULL ref case-insensitively -- typing "local" or "claude" both work -- and
// a provider with zero matching models is omitted entirely, headers included:
// with a large discovered catalog (OpenRouter serves hundreds) the point of
// the grouping is that whole providers collapse away under the filter.
std::vector<ModelPickerRow> BuildModelPickerRows(
    const std::vector<v1::ModelSummary>& models, const std::string& filter);

}  // namespace minimal::client::ui_kit
