#include "minimal/client/view/filterable_menu.hpp"

#include <algorithm>

#include <ftxui/component/component_options.hpp>

#include "minimal/client/utf8.hpp"

namespace minimal::client {

ftxui::Component MakeFilterableMenuModal(
    std::function<std::vector<MenuRow>(const std::string& filter)> build_rows,
    const FilterableMenuSpec& spec, FilterableMenuCallbacks callbacks, const Theme& theme) {
    using namespace ftxui;

    // ALL mutable state lives in one shared block owned by every closure
    // below. The historical bug this guards against (session_picker.cpp's
    // original form): a stack-local `selected` and a by-value-captured
    // index froze the highlight at row 0 regardless of where the cursor
    // actually was -- the closures outlive this factory call, so everything
    // they touch must too.
    struct MenuState {
        std::vector<std::string> labels;   // Menu's own label list, rebuilt on filter change
        std::vector<MenuRow> rows;          // parallel to labels
        int selected = 0;
        std::string filter;
    };
    auto state = std::make_shared<MenuState>();

    auto rebuild = [state, build_rows] {
        state->rows = build_rows(state->filter);
        state->labels.clear();
        state->labels.reserve(state->rows.size());
        for (const auto& row : state->rows) state->labels.push_back(row.label);
        const int last = static_cast<int>(state->labels.size()) - 1;
        state->selected = std::clamp(state->selected, 0, std::max(last, 0));
    };
    rebuild();

    auto menu = Menu(&state->labels, &state->selected);

    auto interactive = CatchEvent(menu, [state, callbacks, rebuild](const Event& event) {
        if (event == Event::Return) {
            if (!state->rows.empty() && callbacks.on_select) {
                callbacks.on_select(state->rows[state->selected]);
            }
            return true;   // consumed either way: no double-submit on empty list
        }
        if (event == Event::Escape) {
            if (callbacks.on_close) callbacks.on_close();
            return true;
        }
        if (event == Event::Backspace) {
            if (!state->filter.empty()) {
                state->filter = TruncateUtf8(state->filter, state->filter.size() - 1);
                rebuild();
            }
            return true;
        }
        if (callbacks.on_extra_key) {
            const MenuContext ctx{state->rows, state->selected, state->filter};
            if (callbacks.on_extra_key(event, ctx, rebuild)) return true;
        }
        if (event.is_character()) {
            state->filter += event.character();
            rebuild();
            return true;
        }
        return false;   // arrow keys go to Menu
    });

    return Renderer(interactive, [interactive, state, spec, theme] {
        using namespace ftxui;
        Elements rows;
        rows.push_back(text(spec.header_label) | bold);
        rows.push_back(text(spec.hint_line) | dim);
        std::string status = " filter: " + (state->filter.empty() ? "(none)" : state->filter);
        if (spec.status_suffix) status += spec.status_suffix(state->filter);
        rows.push_back(text(status) | dim);
        rows.push_back(separator());
        if (state->rows.size() <= spec.empty_threshold) {
            rows.push_back(text(spec.empty_message) | dim);
        }
        rows.push_back(interactive->Render() | vscroll_indicator | frame | flex);
        return window(text(spec.window_title), vbox(std::move(rows)) | size(HEIGHT, LESS_THAN, 20)) |
               color(theme.ColorFor("border_focused"));
    });
}

}  // namespace minimal::client
