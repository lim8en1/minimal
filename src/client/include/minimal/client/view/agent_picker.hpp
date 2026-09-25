#pragma once

// The agent preset picker: a modal listing agents/*.md presets fetched once
// via ListAgentPresets, with a type-to-filter box. Mirrors session_picker.hpp
// minus what does not apply here -- no "+ new" row, no delete, no sort
// toggle, no pagination (the preset list is small and fetched whole).

#include <functional>
#include <string>
#include <vector>

#include <ftxui/component/component.hpp>

#include "minimal/client/render/theme.hpp"
#include "minimal/client/view/filterable_menu.hpp"
#include "minimal/v1/session_service.pb.h"

namespace minimal::client {

struct AgentPickerCallbacks {
    std::function<void(const std::string& name)> on_select;
    std::function<void()> on_close;
};

ftxui::Component MakeAgentPresetPicker(const std::vector<v1::AgentPresetSummary>& presets,
                                       const Theme& theme, AgentPickerCallbacks callbacks,
    MenuStatus status = {});

}  // namespace minimal::client
