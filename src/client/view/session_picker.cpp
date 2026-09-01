#include "minimal/client/view/session_picker.hpp"

#include <ftxui/component/event.hpp>

#include <ftxui/component/component_options.hpp>

namespace minimal::client {

namespace {

std::string HumanTime(std::int64_t unix_ms) {
    if (unix_ms <= 0) return "";
    const std::time_t seconds = static_cast<std::time_t>(unix_ms / 1000);
    std::tm local{};
    localtime_r(&seconds, &local);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &local);
    return buffer;
}

}  // namespace

ftxui::Component MakeSessionPicker(const std::vector<v1::SessionSummary>& sessions,
                                   const Theme& theme, SessionPickerCallbacks callbacks) {
    using namespace ftxui;

    // ALL mutable picker state lives in one shared block owned by BOTH
    // closures. The first draft passed Menu a stack-local `int selected` and a
    // vector whose owning shared_ptr no surviving closure captured: the menu
    // read freed memory for its labels, wrote freed stack on every arrow key,
    // and the CatchEvent's by-value capture of `selected` froze it at 0 -- so
    // Enter always resumed row 0 regardless of the highlight. The closures
    // outlive the factory; everything they touch must too.
    struct PickerState {
        std::vector<std::string> rows;   // menu labels
        std::vector<std::string> ids;    // session id per row
        int selected = 0;
    };
    auto state = std::make_shared<PickerState>();
    for (const auto& session : sessions) {
        state->rows.push_back(session.name() + "  ·  " +
                              std::to_string(session.message_count()) + " msgs  ·  " +
                              HumanTime(session.updated_at_unix_ms()));
        state->ids.push_back(session.session_id());
    }

    auto menu = Menu(&state->rows, &state->selected);

    auto interactive = CatchEvent(menu, [state, callbacks](const ftxui::Event& event) {
        if (event == ftxui::Event::Return) {
            if (!state->ids.empty()) callbacks.on_resume(state->ids[state->selected]);
            return true;   // consumed either way: no double-submit on empty list
        }
        if (event == ftxui::Event::Delete || event == ftxui::Event::Character("d")) {
            // Delete asks for its own confirmation (RunTui stages the modal);
            // the picker only nominates the row.
            if (!state->ids.empty()) callbacks.on_delete(state->ids[state->selected]);
            return true;
        }
        if (event == ftxui::Event::Escape) {
            callbacks.on_close();
            return true;
        }
        return false;
    });

    return Renderer(interactive, [interactive, state, theme] {
        using namespace ftxui;
        Elements rows;
        rows.push_back(text(" sessions ") | bold);
        rows.push_back(text(" enter: resume  ·  d: delete  ·  esc: close") | dim);
        rows.push_back(separator());
        if (state->ids.empty()) {
            rows.push_back(text(" (no stored sessions here)") | dim);
        }
        rows.push_back(interactive->Render() | vscroll_indicator | frame | flex);
        return window(text("resume a session"), vbox(std::move(rows)) | size(HEIGHT, LESS_THAN, 20)) |
               color(theme.ColorFor("border_focused"));
    });
}

}  // namespace minimal::client
