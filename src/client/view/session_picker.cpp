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

    auto state = std::make_shared<std::vector<std::string>>();   // row labels
    auto ids = std::make_shared<std::vector<std::string>>();
    for (const auto& session : sessions) {
        state->push_back(session.name() + "  ·  " + std::to_string(session.message_count()) +
                         " msgs  ·  " + HumanTime(session.updated_at_unix_ms()));
        ids->push_back(session.session_id());
    }

    int selected = 0;
    auto menu = Menu(state.get(), &selected);

    auto interactive = CatchEvent(menu, [=](const ftxui::Event& event) {
        if (event == ftxui::Event::Return) {
            if (!ids->empty()) callbacks.on_resume((*ids)[selected]);
            return true;   // consumed either way: no double-submit on empty list
        }
        if (event == ftxui::Event::Delete || event == ftxui::Event::Character("d")) {
            // Delete asks for its own confirmation (RunTui stages the modal);
            // the picker only nominates the row.
            if (!ids->empty()) callbacks.on_delete((*ids)[selected]);
            return true;
        }
        if (event == ftxui::Event::Escape) {
            callbacks.on_close();
            return true;
        }
        return false;
    });

    return Renderer(interactive, [interactive, ids, theme] {
        using namespace ftxui;
        Elements rows;
        rows.push_back(text(" sessions ") | bold);
        rows.push_back(text(" enter: resume  ·  d: delete  ·  esc: close") | dim);
        rows.push_back(separator());
        if (ids->empty()) {
            rows.push_back(text(" (no stored sessions here)") | dim);
        }
        rows.push_back(interactive->Render() | vscroll_indicator | frame | flex);
        return window(text("resume a session"), vbox(std::move(rows)) | size(HEIGHT, LESS_THAN, 20)) |
               color(theme.ColorFor("border_focused"));
    });
}

}  // namespace minimal::client
