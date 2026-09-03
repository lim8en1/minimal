#include "minimal/client/view/tab_bar.hpp"

#include <algorithm>
#include <array>
#include <chrono>

#include <ftxui/screen/terminal.hpp>

#include "minimal/client/utf8.hpp"

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

ftxui::Color StateColor(SessionStateKind state, bool active, const Theme& theme) {
    switch (state) {
        case SessionStateKind::Waiting:  return theme.ColorFor("attention");
        case SessionStateKind::Error:    return theme.ColorFor("error");
        case SessionStateKind::Done:     return theme.ColorFor("system_message");
        // border_unfocused is a BORDER role (1.70:1 dark / 1.16:1 light
        // against status_bar_bg -- fine for a hairline, useless as body
        // text), but this was the only place in the app using it as a tab
        // LABEL color; combined with the `dim` modifier below it made idle
        // tabs unreadable. system_message/assistant_message are the roles
        // every other body text in the app already uses for exactly this
        // idle/active distinction.
        case SessionStateKind::Idle:
            return active ? theme.ColorFor("assistant_message") : theme.ColorFor("system_message");
        // Thinking and Working used to share this color, making them
        // visually identical (the reported complaint: no way to tell "the
        // model is composing a reply" from "a tool is running" without
        // opening the tools panel). Distinct roles now; StateGlyph below
        // gives them a distinct SHAPE too, so the difference survives even
        // on a theme that hasn't customized either role from its default.
        case SessionStateKind::Thinking: return theme.ColorFor("thinking");
        case SessionStateKind::Working:  return theme.ColorFor("working");
    }
    return ftxui::Color::Default;
}

// Animated for Thinking/Working, static otherwise. Computed from wall clock
// at RENDER time rather than stored per-tab: storing a frame index would
// need a dedicated timer to advance it, and the frame pump already redraws
// continuously while a tab is actually Thinking/Working (every delta bumps
// ui_version_) -- an idle tab never asks for this branch, so no animation
// ever needs a redraw nothing else already provides. Waiting keeps its own
// '!' glyph (RenderTabCell's attention_glyph, unchanged) -- this function
// never emits a second signal for it.
constexpr std::array<const char*, 8> kSpinnerFrames = {"⠋", "⠙", "⠹", "⠸",
                                                       "⠼", "⠴", "⠦", "⠧"};

std::string StateGlyph(SessionStateKind state) {
    switch (state) {
        case SessionStateKind::Thinking:
        case SessionStateKind::Working: {
            using namespace std::chrono;
            const auto ms = duration_cast<milliseconds>(steady_clock::now().time_since_epoch());
            const auto frame = static_cast<std::size_t>(ms.count() / 120) % kSpinnerFrames.size();
            return kSpinnerFrames[frame];
        }
        case SessionStateKind::Done:  return "✓";
        case SessionStateKind::Error: return "✗";
        case SessionStateKind::Waiting:
        case SessionStateKind::Idle:
            return "";
    }
    return "";
}

}  // namespace

namespace {

// " │ ", the separator rendered between adjacent tab cells so labels can
// never fuse into one run-together string.
constexpr int kSeparatorWidth = 3;

// Digits in a 1-based tab index, for the label prefix's width.
int DigitWidth(std::size_t n) {
    int digits = 1;
    for (std::size_t v = n; v >= 10; v /= 10) ++digits;
    return digits;
}

// One tab's label -- "<index> <name>", name column-truncated (see
// utf8.hpp: codepoint-width, not byte length, so a CJK/emoji name isn't cut
// to a fraction of the visible characters an ASCII name of the same budget
// would get) -- with room reserved for the attention glyph, and its
// rendered width in display columns.
std::pair<std::string, int> LabelAndWidth(const AppState::TabSnapshot& tab, std::size_t index) {
    const std::string prefix = std::to_string(index) + " ";
    std::string label = prefix + TruncateUtf8Columns(tab.display_name, 16);
    int width = DigitWidth(index) + 1 + std::min<int>(16, static_cast<int>(Utf8ColumnWidth(
                                                              tab.display_name)));
    if (tab.attention_glyph != '\0') {
        label += ' ';
        width += 2;  // the separating space plus the glyph itself
    }
    if (tab.is_subagent) width += 1;                  // the indent space
    // The state glyph, when this state has one: one display column plus a
    // separating space, approximated as a flat +2 rather than the glyph's
    // own (multi-byte, UTF-8) string length -- StateGlyph's glyphs are all
    // single-width, so byte length would OVERcount and reserve more room
    // than the glyph actually occupies.
    if (!StateGlyph(tab.state).empty()) width += 2;
    // One separator (" │ ", 3 columns) reserved per tab so adjacent labels
    // never fuse into one run-together string -- the actual separator is
    // only rendered BETWEEN visible cells (n-1 of them), so this slightly
    // over-reserves budget for the last visible tab; a cosmetic non-issue,
    // the same tolerance ComputeTabBarWindow's fixed marker width already
    // accepts.
    width += kSeparatorWidth;
    return {std::move(label), width};
}

ftxui::Element RenderTabCell(const AppState::TabSnapshot& tab, const std::string& label,
                             const Theme& theme) {
    using namespace ftxui;
    const bool active = tab.is_focused;

    Element content = text(label);
    content |= color(StateColor(tab.state, active, theme));
    // `dim` stacking on an already-borderline color was exactly what made an
    // idle, unfocused tab (border_unfocused, 1.70:1 dark / 1.16:1 light)
    // unreadable -- now that Idle uses a legible role on its own
    // (StateColor above), it no longer needs `dim` to read as "not focused".
    if (active) {
        content |= bold;
    } else if (tab.state != SessionStateKind::Idle) {
        content |= dim;
    }

    const std::string glyph = StateGlyph(tab.state);
    if (!glyph.empty()) {
        content = hbox({text(glyph + " ") | color(StateColor(tab.state, active, theme)),
                        std::move(content)});
    }
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
        auto [label, width] = LabelAndWidth(tabs[i], i + 1);
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
        if (!cells.empty()) cells.push_back(text(" │ ") | dim);  // never let labels fuse
        cells.push_back(RenderTabCell(tabs[i], labels[i], theme));
    }
    if (window.hidden_after > 0) {
        cells.push_back(text(" " + std::to_string(window.hidden_after) + "»") | dim);
    }
    return hbox(std::move(cells));
}

}  // namespace minimal::client
