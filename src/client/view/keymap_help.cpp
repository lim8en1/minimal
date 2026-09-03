#include "minimal/client/view/keymap_help.hpp"

#include "minimal/client/keymap_help_model.hpp"
#include "minimal/client/view/filterable_menu.hpp"

namespace minimal::client {

ftxui::Component MakeKeymapHelpView(const std::unordered_map<std::string, std::string>& bindings,
                                    const Theme& theme, std::function<void()> on_close) {
    // BY VALUE, not by reference: `bindings` is this factory's OWN
    // parameter, and the returned Component (and its closures) outlive this
    // call -- the exact "closures outlive the factory" trap agent_picker.cpp
    // documents for `presets`.
    auto build_rows = [bindings](const std::string& filter) {
        std::vector<MenuRow> rows;
        for (const auto& row : BuildKeymapHelpRows(bindings, filter)) {
            rows.push_back({row.label, row.action});
        }
        return rows;
    };

    const FilterableMenuSpec spec{
        .window_title = "keybindings",
        .hint_line = " esc: close",
        .status_suffix = nullptr,
        .empty_message = " (no bindings match)",
        .empty_threshold = 0,
    };

    // Nothing to select: Enter closes the overlay exactly like Escape --
    // this is a reference, not a picker.
    const FilterableMenuCallbacks callbacks{
        .on_select = [on_close](const MenuRow&) { if (on_close) on_close(); },
        .on_close = on_close,
        .on_extra_key = nullptr,
    };

    return MakeFilterableMenuModal(build_rows, spec, callbacks, theme);
}

}  // namespace minimal::client
