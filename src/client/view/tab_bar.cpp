#include "minimal/client/view/tab_bar.hpp"

#include <algorithm>

namespace minimal::client {

namespace {

ftxui::Color StateColor(SessionStateKind state, const Theme& theme) {
    switch (state) {
        case SessionStateKind::Waiting: return theme.ColorFor("attention");
        case SessionStateKind::Error:   return theme.ColorFor("error");
        case SessionStateKind::Done:    return theme.ColorFor("system_message");
        case SessionStateKind::Idle:    return theme.ColorFor("border_unfocused");
        case SessionStateKind::Thinking:
        case SessionStateKind::Working: return theme.ColorFor("assistant_message");
    }
    return ftxui::Color::Default;
}

std::string Truncate(const std::string& name, std::size_t max) {
    // Code-point truncation (ASCII-safe cut of a UTF-8 string would split a
    // code point): count continuation bytes 0b10xxxxxx toward the length.
    if (name.size() <= max) return name;
    std::size_t end = max;
    while (end > 0 && (static_cast<unsigned char>(name[end]) & 0xC0) == 0x80) --end;
    return name.substr(0, end);
}

}  // namespace

ftxui::Element RenderTabBar(const std::vector<AppState::TabSnapshot>& tabs, const Theme& theme) {
    using namespace ftxui;
    std::vector<Element> cells;
    for (const auto& tab : tabs) {
        std::string label = Truncate(tab.display_name, 16);
        if (tab.attention_glyph != '\0') label += ' ';  // room for the glyph
        const bool active = tab.is_focused;

        Element content = text(label);
        content |= color(StateColor(tab.state, theme));
        content |= (active ? bold : dim);
        if (tab.attention_glyph != '\0') {
            // THE color-independent signal. Not a spinner, not a state word:
            // Waiting is the one state a human must act on.
            content = hbox({std::move(content), text("!") | color(theme.ColorFor("attention")) |
                                                   bold});
        }
        // Grouping is conveyed by ORDER only (children directly follow their
        // parent); one space of indent is the whole subagent marker.
        if (tab.is_subagent) content = hbox({text(" "), std::move(content)});

        if (active) content |= bgcolor(theme.ColorFor("status_bar_bg"));
        cells.push_back(std::move(content));
    }
    if (cells.empty()) return emptyElement();
    return hbox(std::move(cells));
}

}  // namespace minimal::client
