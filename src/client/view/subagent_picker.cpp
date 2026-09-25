#include "minimal/client/view/subagent_picker.hpp"

#include "minimal/client/subagent_picker_model.hpp"
#include "minimal/client/view/filterable_menu.hpp"

namespace minimal::client {

ftxui::Component MakeSubagentPicker(const std::vector<v1::SessionSummary>& children,
                                    bool include_ended, const Theme& theme,
                                    SubagentPickerCallbacks callbacks, MenuStatus status) {
    auto build_rows = [children, include_ended](const std::string& filter) {
        std::vector<MenuRow> rows;
        for (const auto& row : BuildSubagentPickerRows(children, include_ended, filter)) {
            rows.push_back({row.label, row.session_id});
        }
        return rows;
    };

    FilterableMenuSpec spec{
        .window_title = "subagents",
        .hint_line = " enter: open  ·  ctrl+a: " +
                     std::string(include_ended ? "hide ended" : "show ended") +
                     "  ·  esc: close",
        .status_suffix = nullptr,
        .empty_message = include_ended ? " (no subagents)"
                                       : " (no running subagents -- ctrl+a shows ended ones)",
        .empty_threshold = 0,
    };

    FilterableMenuCallbacks callbacks_out{
        .on_select =
            [callbacks, children](const MenuRow& row) {
                // The rows carry no is_live, and the model's build is a pure
                // transform -- re-derive the flag from the captured list the
                // same way the session picker re-derives is_live in
                // PickerController (one linear scan over a small list).
                for (const auto& child : children) {
                    if (child.session_id() == row.id) {
                        if (callbacks.on_select) callbacks.on_select(row.id, child.is_live());
                        return;
                    }
                }
            },
        .on_close = callbacks.on_close,
        .on_extra_key =
            [callbacks](const ftxui::Event& event, const MenuContext&,
                        const std::function<void()>&) {
                using ftxui::Event;
                // Same hardcoded in-picker key as the session picker's
                // all-dirs toggle (session_picker.cpp): modal keys are not
                // keybindings.json actions.
                if (event == Event::CtrlA) {
                    if (callbacks.on_toggle_ended) callbacks.on_toggle_ended();
                    return true;
                }
                return false;
            },
    };

    FilterableMenuSpec with_status = spec;

    with_status.status = std::move(status);

    return MakeFilterableMenuModal(build_rows, with_status, callbacks_out, theme);
}

}  // namespace minimal::client
