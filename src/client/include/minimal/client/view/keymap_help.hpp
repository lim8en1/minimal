#pragma once

// The keymap help overlay: a modal listing every bound action and its key,
// with a type-to-filter box, opened by help.overlay (default F1). Mirrors
// agent_picker.hpp minus selection -- there is nothing to switch to, Enter
// and Escape both just close it.

#include <functional>
#include <string>
#include <unordered_map>

#include <ftxui/component/component.hpp>

#include "minimal/client/render/theme.hpp"

namespace minimal::client {

ftxui::Component MakeKeymapHelpView(const std::unordered_map<std::string, std::string>& bindings,
                                    const Theme& theme, std::function<void()> on_close);

}  // namespace minimal::client
