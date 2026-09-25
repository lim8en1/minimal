#include "minimal/client/view/session_picker.hpp"

#include "minimal/client/picker_model.hpp"
#include "minimal/client/view/filterable_menu.hpp"

namespace minimal::client {

ftxui::Component MakeSessionPicker(const std::vector<v1::SessionSummary>& sessions,
                                   bool include_all_dirs, bool has_more, const Theme& theme,
                                   SessionPickerCallbacks callbacks, MenuStatus status) {
    // Sort is this picker's own display-only state (picker_model.hpp: a
    // pure transform, no host round trip) -- lives outside MenuState, which
    // filterable_menu.cpp owns and knows nothing about sort.
    auto sort = std::make_shared<PickerSort>(PickerSort::kRecent);

    auto build_rows = [sessions, sort](const std::string& filter) {
        std::vector<MenuRow> rows;
        for (const auto& row : BuildPickerRows(sessions, filter, *sort)) {
            rows.push_back({row.label, row.session_id});
        }
        return rows;
    };

    FilterableMenuSpec spec{
        .window_title = "resume a session",
        .hint_line = " enter: open  ·  ctrl+d: delete  ·  tab: sort  ·  "
                    "ctrl+a: all dirs  ·  esc: close",
        .status_suffix =
            [sort, include_all_dirs, has_more](const std::string&) {
                std::string suffix =
                    *sort == PickerSort::kRecent ? "  ·  sort: recent" : "  ·  sort: name";
                suffix += include_all_dirs ? "  ·  all dirs" : "  ·  this project";
                if (has_more) suffix += "  ·  more below (↓ to load)";
                return suffix;
            },
        .empty_message = " (no other stored sessions here)",
        .empty_threshold = 1,   // the synthetic "+ new session" row is always present
    };

    FilterableMenuCallbacks callbacks_out{
        .on_select =
            [callbacks](const MenuRow& row) {
                if (row.id.empty()) {
                    if (callbacks.on_new) callbacks.on_new();
                } else if (callbacks.on_resume) {
                    callbacks.on_resume(row.id);
                }
            },
        .on_close = callbacks.on_close,
        .on_extra_key =
            [callbacks, sort, has_more](const ftxui::Event& event, const MenuContext& ctx,
                                        const std::function<void()>& rebuild) {
                using ftxui::Event;
                // ctrl+d: delete the highlighted row. Moved off bare 'd' (T8)
                // now that a bare character types into the filter; the
                // new-session row has nothing to delete.
                if (event == Event::CtrlD) {
                    if (!ctx.rows.empty()) {
                        const auto& row = ctx.rows[ctx.selected];
                        if (!row.id.empty() && callbacks.on_delete) callbacks.on_delete(row.id);
                    }
                    return true;
                }
                // Tab: cycle the sort order. A pure local rebuild -- no host
                // round trip, unlike ctrl+a below, because sort never
                // changes what was fetched, only how it is displayed.
                if (event == Event::Tab) {
                    *sort = *sort == PickerSort::kRecent ? PickerSort::kName : PickerSort::kRecent;
                    rebuild();
                    return true;
                }
                // ctrl+a: toggle include_all_dirs. This DOES need the host:
                // it changes what the next ListSessions call should ask the
                // server for.
                if (event == Event::CtrlA) {
                    if (callbacks.on_toggle_all_dirs) callbacks.on_toggle_all_dirs();
                    return true;
                }
                // Arrow-down off the last loaded row asks the host for
                // another page (before Menu's own handling, which would
                // otherwise just clamp at the end and never signal anything
                // happened). Filtered views don't trigger it: "load more"
                // fetches sessions the SERVER hasn't sent yet, which has
                // nothing to do with rows a local filter is hiding.
                if (event == Event::ArrowDown && has_more && ctx.filter.empty() &&
                    !ctx.rows.empty() && ctx.selected == static_cast<int>(ctx.rows.size()) - 1) {
                    if (callbacks.on_load_more) callbacks.on_load_more();
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
