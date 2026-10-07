#include "model_picker.hpp"

#include "model_picker_model.hpp"
#include "filterable_menu.hpp"

namespace minimal::client::ui_kit {

ftxui::Component MakeModelPicker(const std::vector<v1::ModelSummary>& models,
                                 const Theme& theme, ModelPickerCallbacks callbacks, MenuStatus status) {
    auto build_rows = [models, callbacks](const std::string& filter) {
        std::vector<MenuRow> rows;
        // The leading row first, so it tops the list and survives every
        // filter (it is not a model; the filter text never applies to it).
        if (!callbacks.leading_row_id.empty()) {
            rows.push_back({callbacks.leading_row_label, callbacks.leading_row_id,
                            /*header=*/false});
        }
        bool exact = false;
        for (const auto& row : BuildModelPickerRows(models, filter)) {
            rows.push_back({row.label, row.ref, row.header});
            exact = exact || (!row.header && row.ref == filter);
        }
        if (callbacks.allow_custom && !exact && !filter.empty() &&
            (callbacks.custom_is_name || filter.find('/') != std::string::npos) &&
            filter.find(' ') == std::string::npos) {
            rows.push_back({"use '" + filter + "' (not listed)", filter, /*header=*/false});
        }
        return rows;
    };

    FilterableMenuSpec spec{
        .window_title = callbacks.window_title,
        .hint_line = " enter: " + callbacks.select_hint +
                     "  ·  ctrl+r: refresh discovery  ·  esc: close",
        .empty_message = " (no models match)",
        // The leading row is always present; without this the "no models
        // match" message could never fire (rows is never empty).
        .empty_threshold = callbacks.leading_row_id.empty() ? std::size_t{0} : std::size_t{1},
    };

    FilterableMenuCallbacks callbacks_out{
        .on_select =
            [callbacks](const MenuRow& row) {
                // A provider header is a label, not a choice: Enter on one is
                // a no-op (the shell guarantees on_select only fires with a
                // row highlighted). The cursor never RESTS on a header either
                // -- the shell walks the selection to the nearest selectable
                // row on every rebuild and render pass -- so this guard is
                // for the one transient frame a move can pass through.
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

    FilterableMenuSpec with_status = spec;

    with_status.status = std::move(status);

    return MakeFilterableMenuModal(build_rows, with_status, callbacks_out, theme);
}

}  // namespace minimal::client::ui_kit
