#pragma once

// Core's vocabulary for its own key actions: what each does, in words, and
// where it is listed. What HostUi::KeyBindings hands a plugin listing the
// keymap (help-view); the listing itself is the plugin's.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace minimal::client {

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
// `rank`: the action's place within its group when its owner declared an
// order (the prompt editor's actions, in the order it lists them).
KeyActionInfo DescribeKeyAction(std::string_view action,
                                std::optional<std::size_t> rank = std::nullopt);

}  // namespace minimal::client
