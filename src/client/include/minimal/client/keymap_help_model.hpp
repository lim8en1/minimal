#pragma once

// Pure row-building logic for the keymap help overlay (?/F1): (bindings,
// filter) -> rows, no FTXUI. Mirrors agent_picker_model.hpp's split.

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace minimal::client {

struct KeymapHelpRow {
    // A binding: "  alt+down  next tab" -- the KEY first, in a column, then
    // what it does. A header: the group name ("tabs", "chat", ...).
    std::string label;
    std::string action;   // the action id (empty for a header); nothing is "selected"
    bool header = false;
};

// What a core action does, in words ("next tab"); an action this build has
// no description for (a plugin view's toggle) is described by its own id.
std::string ActionDescription(std::string_view action);

// Core's word on one key action, for HostUi::KeyBindings: what it does, the
// group it is listed under ("tabs", "chat", ...; "views" for anything core
// does not know -- a plugin view's toggle) and a sort key that orders the
// groups, and the actions within one, as they read best.
struct KeyActionInfo {
    std::string description;
    std::string group;
    int order = 0;
};
KeyActionInfo DescribeKeyAction(std::string_view action);

// Every binding whose action id, key, or description contains `filter`
// case-insensitively, grouped by area (tabs, chat, turn, open, panels, app,
// then plugin actions) under a header row per non-empty group, sorted by
// action id within a group -- people look for "how do I scroll", not for
// "chat.scroll_up", so the listing reads by area and by key.
std::vector<KeymapHelpRow> BuildKeymapHelpRows(
    const std::unordered_map<std::string, std::string>& bindings, const std::string& filter);

}  // namespace minimal::client
