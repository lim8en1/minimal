#include "minimal/client/view/model_picker.hpp"

#include "minimal/client/model_picker_model.hpp"
#include "minimal/client/view/filterable_menu.hpp"

namespace minimal::client {

ftxui::Component MakeModelPicker(const std::vector<v1::ModelSummary>& models,
                                 const Theme& theme, ModelPickerCallbacks callbacks) {
    auto build_rows = [models](const std::string& filter) {
        std::vector<MenuRow> rows;
        for (const auto& row : BuildModelPickerRows(models, filter)) {
            rows.push_back({row.label, row.ref, row.header});
        }
        return rows;
    };

    FilterableMenuSpec spec{
        .window_title = "switch model",
        .hint_line = " enter: switch  ·  ctrl+r: refresh discovery  ·  esc: close",
        .empty_message = " (no models match)",
    };

    FilterableMenuCallbacks callbacks_out{
        .on_select =
            [callbacks](const MenuRow& row) {
                // A provider header is a label, not a choice: Enter on one is
                // a no-op (the shell guarantees on_select only fires with a
                // row highlighted; headers ARE highlightable -- skipping them
                // in Menu's own navigation would mean forgoing FTXUI's Menu
                // for a hand-rolled list).
                if (row.header || row.id.empty()) return;
                if (callbacks.on_select) callbacks.on_select(row.id);
            },
        .on_close = callbacks.on_close,
        .on_extra_key =
            [callbacks](const ftxui::Event& event, const MenuContext&,
                        const std::function<void()>&) {
                // ctrl+r: discovery refresh. Purely the host's job -- it
                // re-fetches (the fetch is synchronous and can take seconds)
                // and rebuilds this component in place; nothing to mutate in
                // the menu's own state.
                if (event == ftxui::Event::CtrlR) {
                    if (callbacks.on_refresh_discovery) callbacks.on_refresh_discovery();
                    return true;
                }
                return false;
            },
    };

    return MakeFilterableMenuModal(build_rows, spec, callbacks_out, theme);
}

}  // namespace minimal::client
