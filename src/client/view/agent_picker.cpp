#include "minimal/client/view/agent_picker.hpp"

#include <algorithm>

#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>

#include "minimal/client/agent_picker_model.hpp"
#include "minimal/client/utf8.hpp"

namespace minimal::client {

ftxui::Component MakeAgentPresetPicker(const std::vector<v1::AgentPresetSummary>& presets,
                                       const Theme& theme, AgentPickerCallbacks callbacks) {
    using namespace ftxui;

    // Same reasoning as the session picker's PickerState: every closure below
    // outlives this factory call, so all mutable state lives in one shared
    // block rather than on the stack.
    struct PickerState {
        std::vector<std::string> labels;
        std::vector<AgentPickerRow> rows;
        int selected = 0;
        std::string filter;
    };
    auto state = std::make_shared<PickerState>();

    // `presets` captured by VALUE: the caller's vector may not outlive the
    // modal, and rebuild runs on every filter keystroke.
    auto rebuild = [state, presets] {
        state->rows = BuildAgentPickerRows(presets, state->filter);
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
                callbacks.on_select(state->rows[state->selected].name);
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
        if (event.is_character()) {
            state->filter += event.character();
            rebuild();
            return true;
        }
        return false;   // arrow keys go to Menu
    });

    return Renderer(interactive, [interactive, state, theme] {
        using namespace ftxui;
        Elements rows;
        rows.push_back(text(" agents ") | bold);
        rows.push_back(text(" enter: switch  ·  esc: close") | dim);
        std::string status = " filter: " + (state->filter.empty() ? "(none)" : state->filter);
        rows.push_back(text(status) | dim);
        rows.push_back(separator());
        if (state->rows.empty()) {
            rows.push_back(text(" (no agent presets match)") | dim);
        }
        rows.push_back(interactive->Render() | vscroll_indicator | frame | flex);
        return window(text("switch agent preset"), vbox(std::move(rows)) | size(HEIGHT, LESS_THAN, 20)) |
               color(theme.ColorFor("border_focused"));
    });
}

}  // namespace minimal::client
