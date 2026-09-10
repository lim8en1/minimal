#pragma once

// The theme picker modal: a flat filterable menu over the themes dir's
// *.json files (stems as names). Same "modal over data it does not own"
// shape as MakeModelPicker: the row building is pure, selection just
// reports the chosen name -- the swap itself (Theme::Load, cache
// invalidation, plugin re-push, ui.json persistence) is the caller's
// /theme dispatch path, exactly as the named form runs it.

#include <functional>
#include <string>
#include <vector>

#include <ftxui/component/component.hpp>

#include "minimal/client/render/theme.hpp"

namespace minimal::client {

struct ThemePickerCallbacks {
    std::function<void(const std::string& name)> on_select;
    std::function<void()> on_close;   // Escape
};

// `names` is the caller's theme enumeration (config themes dir stems);
// `current` is marked on its row so the picker reads as a switch, not a
// fresh choice. Both are copied: the component outlives the caller's
// locals in the modal-host pattern.
ftxui::Component MakeThemePicker(const std::vector<std::string>& names,
                                 const std::string& current, const Theme& theme,
                                 ThemePickerCallbacks callbacks);

}  // namespace minimal::client
