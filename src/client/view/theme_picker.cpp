#include "minimal/client/view/theme_picker.hpp"

#include "minimal/client/view/filterable_menu.hpp"

namespace minimal::client {

ftxui::Component MakeThemePicker(const std::vector<std::string>& names,
                                 const std::string& current, const Theme& theme,
                                 ThemePickerCallbacks callbacks) {
    auto build_rows = [names, current](const std::string& filter) {
        std::vector<MenuRow> rows;
        for (const auto& name : names) {
            if (!filter.empty() && name.find(filter) == std::string::npos) continue;
            // The active theme gets a marker: /theme reads as a switch, and a
            // switch shows what it is switched FROM. The id stays the bare
            // name either way -- the swap path takes it verbatim.
            const bool active = name == current;
            rows.push_back({active ? "● " + name : "  " + name, name, /*header=*/false});
        }
        return rows;
    };

    FilterableMenuSpec spec{
        .window_title = "switch theme",
        .hint_line = " enter: switch  ·  esc: close",
        .empty_message = " (no themes match)",
    };

    FilterableMenuCallbacks callbacks_out{
        .on_select =
            [callbacks](const MenuRow& row) {
                if (row.id.empty()) return;
                if (callbacks.on_select) callbacks.on_select(row.id);
            },
        .on_close = callbacks.on_close,
    };

    return MakeFilterableMenuModal(build_rows, spec, callbacks_out, theme);
}

}  // namespace minimal::client
