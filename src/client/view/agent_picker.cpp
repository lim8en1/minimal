#include "minimal/client/view/agent_picker.hpp"

#include "minimal/client/agent_picker_model.hpp"
#include "minimal/client/view/filterable_menu.hpp"

namespace minimal::client {

ftxui::Component MakeAgentPresetPicker(const std::vector<v1::AgentPresetSummary>& presets,
                                       const Theme& theme, AgentPickerCallbacks callbacks) {
    auto build_rows = [presets](const std::string& filter) {
        std::vector<MenuRow> rows;
        for (const auto& row : BuildAgentPickerRows(presets, filter)) {
            rows.push_back({row.label, row.name, /*header=*/false, row.color});
        }
        return rows;
    };

    const FilterableMenuSpec spec{
        .window_title = "switch agent preset",
        .hint_line = " enter: switch  ·  esc: close",
        .status_suffix = nullptr,
        .empty_message = " (no agent presets match)",
        .empty_threshold = 0,   // no synthetic row here
    };

    const FilterableMenuCallbacks callbacks_out{
        .on_select =
            [callbacks](const MenuRow& row) {
                if (callbacks.on_select) callbacks.on_select(row.id);
            },
        .on_close = callbacks.on_close,
        .on_extra_key = nullptr,
    };

    return MakeFilterableMenuModal(build_rows, spec, callbacks_out, theme);
}

}  // namespace minimal::client
