#include "minimal/client/agent_picker_model.hpp"

#include <algorithm>
#include <cctype>

namespace minimal::client {

namespace {

std::string ToLower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool MatchesFilter(const v1::AgentPresetSummary& preset, const std::string& lowered_filter) {
    if (lowered_filter.empty()) return true;
    return ToLower(preset.name()).find(lowered_filter) != std::string::npos ||
           ToLower(preset.description()).find(lowered_filter) != std::string::npos;
}

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
        if (!MatchesFilter(preset, lowered_filter)) continue;
        rows.push_back(AgentPickerRow{Label(preset), preset.name()});
    }
    return rows;
}

}  // namespace minimal::client
