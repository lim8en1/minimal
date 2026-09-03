#pragma once

// Pure row-building logic for the keymap help overlay (?/F1): (bindings,
// filter) -> rows, no FTXUI. Mirrors agent_picker_model.hpp's split.

#include <string>
#include <unordered_map>
#include <vector>

namespace minimal::client {

struct KeymapHelpRow {
    std::string label;   // display text: "action  --  key"
    std::string action;  // the action name -- an id only; nothing is "selected"
};

// Every (action, key) pair whose action or key contains `filter`
// case-insensitively, sorted by action name for a stable, predictable
// listing (the underlying map has no meaningful order of its own).
std::vector<KeymapHelpRow> BuildKeymapHelpRows(
    const std::unordered_map<std::string, std::string>& bindings, const std::string& filter);

}  // namespace minimal::client
