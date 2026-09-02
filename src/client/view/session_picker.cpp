#include "minimal/client/view/session_picker.hpp"

#include <algorithm>

#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>

#include "minimal/client/picker_model.hpp"
#include "minimal/client/utf8.hpp"

namespace minimal::client {

ftxui::Component MakeSessionPicker(const std::vector<v1::SessionSummary>& sessions,
                                   bool include_all_dirs, bool has_more, const Theme& theme,
                                   SessionPickerCallbacks callbacks) {
    using namespace ftxui;

    // ALL mutable picker state lives in one shared block owned by every
    // closure below. The historical bug this guards against: a stack-local
    // `selected` and a by-value-captured index froze the highlight at row 0
    // regardless of where the cursor actually was -- the closures outlive
    // the factory, so everything they touch must too.
    struct PickerState {
        std::vector<std::string> labels;    // Menu's own label list, rebuilt on filter/sort change
        std::vector<PickerRow> rows;         // parallel: session_id per row ("" = new-session row)
        int selected = 0;
        std::string filter;
        PickerSort sort = PickerSort::kRecent;
    };
    auto state = std::make_shared<PickerState>();

    // `sessions` is captured by VALUE (a copy) into this closure: the vector
    // the caller passed in may not outlive the call the way state does, and
    // rebuild runs on every filter/sort keystroke for the life of the modal.
    auto rebuild = [state, sessions] {
        state->rows = BuildPickerRows(sessions, state->filter, state->sort);
        state->labels.clear();
        state->labels.reserve(state->rows.size());
        for (const auto& row : state->rows) state->labels.push_back(row.label);
        const int last = static_cast<int>(state->labels.size()) - 1;
        state->selected = std::clamp(state->selected, 0, std::max(last, 0));
    };
    rebuild();

    auto menu = Menu(&state->labels, &state->selected);

    auto interactive = CatchEvent(menu, [state, callbacks, rebuild, has_more](const Event& event) {
        if (event == Event::Return) {
            if (!state->rows.empty()) {
                const auto& row = state->rows[state->selected];
                if (row.session_id.empty()) {
                    if (callbacks.on_new) callbacks.on_new();
                } else if (callbacks.on_resume) {
                    callbacks.on_resume(row.session_id);
                }
            }
            return true;   // consumed either way: no double-submit on empty list
        }
        if (event == Event::Escape) {
            if (callbacks.on_close) callbacks.on_close();
            return true;
        }
        // ctrl+d: delete the highlighted row. Moved off bare 'd' (T8) now
        // that a bare character types into the filter; the new-session row
        // has nothing to delete.
        if (event == Event::CtrlD) {
            if (!state->rows.empty()) {
                const auto& row = state->rows[state->selected];
                if (!row.session_id.empty() && callbacks.on_delete) {
                    callbacks.on_delete(row.session_id);
                }
            }
            return true;
        }
        // Tab: cycle the sort order. A pure local rebuild -- no host round
        // trip, unlike ctrl+a below, because sort never changes what was
        // fetched, only how it is displayed (picker_model.hpp).
        if (event == Event::Tab) {
            state->sort =
                state->sort == PickerSort::kRecent ? PickerSort::kName : PickerSort::kRecent;
            rebuild();
            return true;
        }
        // ctrl+a: toggle include_all_dirs. This DOES need the host: it
        // changes what the next ListSessions call should ask the server
        // for, and the component holds no client reference of its own.
        if (event == Event::CtrlA) {
            if (callbacks.on_toggle_all_dirs) callbacks.on_toggle_all_dirs();
            return true;
        }
        if (event == Event::Backspace) {
            if (!state->filter.empty()) {
                state->filter = TruncateUtf8(state->filter, state->filter.size() - 1);
                rebuild();
            }
            return true;
        }
        // Arrow-down off the last loaded row asks the host for another page
        // (before Menu's own handling, which would otherwise just clamp at
        // the end and never signal anything happened). Filtered views don't
        // trigger it: "load more" fetches sessions the SERVER hasn't sent
        // yet, which has nothing to do with rows a local filter is hiding.
        // Handled here, not passed to Menu: Menu is already at the bottom and
        // would just no-op (and, depending on its own OnEvent contract,
        // report the event as unconsumed) -- firing the callback IS this
        // event's meaningful effect.
        if (event == Event::ArrowDown && has_more && state->filter.empty() &&
            !state->rows.empty() &&
            state->selected == static_cast<int>(state->rows.size()) - 1) {
            if (callbacks.on_load_more) callbacks.on_load_more();
            return true;
        }
        if (event.is_character()) {
            state->filter += event.character();
            rebuild();
            return true;
        }
        return false;   // arrow keys (besides the load-more case above) go to Menu
    });

    return Renderer(interactive, [interactive, state, include_all_dirs, has_more, theme] {
        using namespace ftxui;
        Elements rows;
        rows.push_back(text(" sessions ") | bold);
        rows.push_back(text(" enter: open  ·  ctrl+d: delete  ·  tab: sort  ·  "
                            "ctrl+a: all dirs  ·  esc: close") |
                       dim);
        std::string status = " filter: " + (state->filter.empty() ? "(none)" : state->filter);
        status += state->sort == PickerSort::kRecent ? "  ·  sort: recent" : "  ·  sort: name";
        status += include_all_dirs ? "  ·  all dirs" : "  ·  this project";
        if (has_more) status += "  ·  more below (↓ to load)";
        rows.push_back(text(status) | dim);
        rows.push_back(separator());
        if (state->rows.size() <= 1) {   // only the new-session row matched
            rows.push_back(text(" (no other stored sessions here)") | dim);
        }
        rows.push_back(interactive->Render() | vscroll_indicator | frame | flex);
        return window(text("resume a session"), vbox(std::move(rows)) | size(HEIGHT, LESS_THAN, 20)) |
               color(theme.ColorFor("border_focused"));
    });
}

}  // namespace minimal::client
