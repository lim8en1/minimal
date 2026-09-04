#pragma once

// Pure row-building logic for the agent preset picker, mirroring
// picker_model.hpp's split for the session picker (T8): (presets, filter) ->
// rows, no FTXUI, no AppState, no network -- over the
// v1::AgentPresetSummary list app.cpp fetches once via ListAgentPresets.

#include <string>
#include <vector>

#include "minimal/v1/session_service.pb.h"

namespace minimal::client {

struct AgentPickerRow {
    std::string label;   // display text: "name  ·  description"
    std::string name;    // the preset name /agent <name> switches to
    std::string color;   // "#rrggbb" from the preset; empty = theme default
};

// Every preset whose name or description contains `filter` case-insensitively,
// in the order `presets` arrived (ListAvailable's own sort, by name -- no
// separate sort toggle here, unlike the session picker, since the list is
// small and has no "recent" axis).
std::vector<AgentPickerRow> BuildAgentPickerRows(const std::vector<v1::AgentPresetSummary>& presets,
                                                 const std::string& filter);

}  // namespace minimal::client
