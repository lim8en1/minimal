#include "minimal/client/agent_picker_model.hpp"

#include "minimal/client/picker_model.hpp"
#include "minimal/client/utf8.hpp"

namespace minimal::client {

namespace {

std::string Label(const v1::AgentPresetSummary& preset) {
    if (preset.description().empty()) return preset.name();
    return preset.name() + "  ·  " + preset.description();
}

}  // namespace

std::vector<AgentPickerRow> BuildAgentPickerRows(const std::vector<v1::AgentPresetSummary>& presets,
                                                 const std::string& filter) {
    const std::string lowered_filter = ToLower(filter);

    std::vector<AgentPickerRow> rows;
    rows.reserve(presets.size());
    for (const auto& preset : presets) {
        if (!MatchesFilter(lowered_filter, {preset.name(), preset.description()})) continue;
        rows.push_back(AgentPickerRow{Label(preset), preset.name(), preset.color()});
    }
    return rows;
}

}  // namespace minimal::client
