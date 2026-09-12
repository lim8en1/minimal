#include "minimal/client/view/mcp_picker.hpp"

#include "minimal/client/mcp_picker_model.hpp"
#include "minimal/client/view/filterable_menu.hpp"

namespace minimal::client {

ftxui::Component MakeMcpPicker(const std::vector<v1::McpServerSummary>& servers,
                               const Theme& theme, McpPickerCallbacks callbacks) {
    auto build_rows = [servers](const std::string& filter) {
        std::vector<MenuRow> rows;
        for (const auto& row : BuildMcpPickerRows(servers, filter)) {
            rows.push_back({row.label, row.name, /*header=*/false});
        }
        return rows;
    };

    FilterableMenuSpec spec{
        .window_title = "mcp servers",
        .hint_line = " enter: enable/disable  ·  esc: close",
        .empty_message = " (no mcp servers configured)",
    };

    FilterableMenuCallbacks callbacks_out{
        .on_select =
            [callbacks](const MenuRow& row) {
                if (row.header || row.id.empty()) return;
                if (callbacks.on_toggle) callbacks.on_toggle(row.id);
            },
        .on_close = callbacks.on_close,
    };

    return MakeFilterableMenuModal(build_rows, spec, callbacks_out, theme);
}

}  // namespace minimal::client
