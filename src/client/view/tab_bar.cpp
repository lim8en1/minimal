#include "minimal/client/view/tab_bar.hpp"

#include <algorithm>

#include <ftxui/screen/terminal.hpp>

namespace minimal::client {

TabBarWindow ComputeTabBarWindow(const std::vector<int>& cell_widths, int available_width,
                                 std::size_t focused_index, int marker_width) {
    const std::size_t n = cell_widths.size();
    TabBarWindow result;
    if (n == 0) return result;
    if (focused_index >= n) focused_index = 0;

    // Does [start, end) fit, given markers reserved for whichever sides are
    // STILL hidden at those exact boundaries? Re-deriving marker need from
    // (start, end) every call -- rather than tracking it separately -- is
    // what makes reclaiming a marker's width, the instant its side reaches
    // the end of the list, fall out for free.
    const auto fits = [&](std::size_t start, std::size_t end) {
        int width = 0;
        for (std::size_t i = start; i < end; ++i) width += cell_widths[i];
        if (start > 0) width += marker_width;
        if (end < n) width += marker_width;
        return width <= available_width;
    };

    std::size_t start = focused_index;
    std::size_t end = focused_index + 1;
    if (!fits(start, end)) {
        // Not even the focused tab plus both markers fits -- degrade to
        // just the focused tab with no markers (an undercount is better
        // than an empty bar, and this is already a terminal narrower than
        // one tab cell plus two markers, an extreme case).
        result.visible_start = start;
        result.visible_end = end;
        result.hidden_before = start;
        result.hidden_after = n - end;
        return result;
    }
    // Grow alternately -- right (what's next) then left -- one tab at a
    // time, re-checking fit() after each successful step so a marker
    // reclaimed by reaching an edge is immediately available to the OTHER
    // side too. Terminates: each iteration either grows the window by at
    // least one or leaves both `grew` branches false.
    bool grew = true;
    while (grew) {
        grew = false;
        if (end < n && fits(start, end + 1)) {
            ++end;
            grew = true;
        }
        if (start > 0 && fits(start - 1, end)) {
            --start;
            grew = true;
        }
    }
    result.visible_start = start;
    result.visible_end = end;
    result.hidden_before = start;
    result.hidden_after = n - end;
    return result;
}

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

namespace {

// One tab's label (Truncate()d, with room reserved for the attention
// glyph) and its approximate rendered width -- byte length, the same
// UTF-8-display-width approximation Truncate() itself already makes, kept
// consistent rather than mixing a byte-based truncation with a
// codepoint-aware width estimate.
std::pair<std::string, int> LabelAndWidth(const AppState::TabSnapshot& tab) {
    std::string label = Truncate(tab.display_name, 16);
    if (tab.attention_glyph != '\0') label += ' ';   // room for the glyph
    int width = static_cast<int>(label.size());
    if (tab.attention_glyph != '\0') width += 1;      // the glyph itself
    if (tab.is_subagent) width += 1;                  // the indent space
    return {std::move(label), width};
}

ftxui::Element RenderTabCell(const AppState::TabSnapshot& tab, const std::string& label,
                             const Theme& theme) {
    using namespace ftxui;
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
    return content;
}

}  // namespace

ftxui::Element RenderTabBar(const std::vector<AppState::TabSnapshot>& tabs, const Theme& theme) {
    using namespace ftxui;
    if (tabs.empty()) return emptyElement();

    std::vector<std::string> labels;
    std::vector<int> widths;
    labels.reserve(tabs.size());
    widths.reserve(tabs.size());
    std::size_t focused_index = 0;
    for (std::size_t i = 0; i < tabs.size(); ++i) {
        auto [label, width] = LabelAndWidth(tabs[i]);
        labels.push_back(std::move(label));
        widths.push_back(width);
        if (tabs[i].is_focused) focused_index = i;
    }

    // "«123 " / " 123»": digits plus the glyph plus a hair of breathing
    // room. Fixed rather than measured per-count -- a count wide enough to
    // overrun this by even one column is a TUI with 1000+ open tabs, an
    // extreme nobody hits in practice, and overrunning a text row by one
    // column is a cosmetic non-event, not a functional break.
    constexpr int kMarkerWidth = 5;
    const auto window = ComputeTabBarWindow(widths, Terminal::Size().dimx, focused_index,
                                            kMarkerWidth);

    Elements cells;
    if (window.hidden_before > 0) {
        cells.push_back(text("«" + std::to_string(window.hidden_before) + " ") | dim);
    }
    for (std::size_t i = window.visible_start; i < window.visible_end; ++i) {
        cells.push_back(RenderTabCell(tabs[i], labels[i], theme));
    }
    if (window.hidden_after > 0) {
        cells.push_back(text(" " + std::to_string(window.hidden_after) + "»") | dim);
    }
    return hbox(std::move(cells));
}

}  // namespace minimal::client
