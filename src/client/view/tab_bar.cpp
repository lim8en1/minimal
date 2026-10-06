#include "minimal/client/view/tab_bar.hpp"

#include <algorithm>
#include <array>
#include <chrono>

#include <ftxui/screen/terminal.hpp>

#include "minimal/client/utf8.hpp"
#include "minimal/client/view/tool_status.hpp"
#include "minimal/client/policy.hpp"

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
        // just the focused tab, with the hidden counts ZEROED so the
        // markers stay suppressed as documented: at this width the
        // markers themselves would eat the one visible cell, and the
        // non-zero counts (an old leftover) made RenderTabBar draw them
        // anyway, contradicting this comment.
        result.visible_start = start;
        result.visible_end = end;
        result.hidden_before = 0;
        result.hidden_after = 0;
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

// Animated for Thinking/Working, static otherwise -- the same spinner and
// the same ✓/✗ the tool rows use (tool_status.hpp), so "busy" and "done"
// read identically on the tab and in the transcript. Waiting keeps its own
// '!' glyph (RenderTabCell's attention_glyph, unchanged) -- this function
// never emits a second signal for it.
std::string StateGlyph(SessionStateKind state) {
    switch (state) {
        case SessionStateKind::Thinking:
        case SessionStateKind::Working:
            return SpinnerFrame();
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
constexpr int kSeparatorWidth = 3;  // NOLINT(readability-magic-numbers): the columns of " │ "

// Digits in a 1-based tab index, for the label prefix's width.
int DigitWidth(std::size_t n) { return static_cast<int>(std::to_string(n).size()); }

// One tab's label -- "<index> <name>", name column-truncated (see
// utf8.hpp: codepoint-width, not byte length, so a CJK/emoji name isn't cut
// to a fraction of the visible characters an ASCII name of the same budget
// would get) -- with room reserved for the attention glyph, and its
// rendered width in display columns.
std::pair<std::string, int> LabelAndWidth(const AppState::TabSnapshot& tab, std::size_t index,
                                          std::size_t name_cap) {
    const std::string prefix = std::to_string(index) + " ";
    std::string label = prefix + TruncateUtf8Columns(tab.display_name, name_cap);
    int width = DigitWidth(index) + 1 +
                std::min<int>(static_cast<int>(name_cap),
                              static_cast<int>(Utf8ColumnWidth(tab.display_name)));
    if (tab.attention_glyph != '\0') {
        label += ' ';
        width += 2;  // the separating space plus the glyph itself
    }
    if (tab.is_subagent) width += 2;                  // the "↳ " marker
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
    // No `dim` for unfocused tabs: SGR dim is terminal-defined and unreadable
    // in practice. Focus reads from bold + the status_bar_bg fill below.
    if (active) content |= bold;

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
    // Children directly follow their parent; "↳" says so -- one space of
    // indent, the old marker, was invisible in practice.
    if (tab.is_subagent) content = hbox({text("↳ ") | Muted(theme), std::move(content)});

    if (active) content |= bgcolor(theme.ColorFor("status_bar_bg"));
    return content;
}

}  // namespace

TabBarBadge ComputeTabBarBadge(const std::vector<AppState::TabSnapshot>& all,
                               const std::vector<AppState::TabSnapshot>& visible,
                               TabFilter filter) {
    if (filter == TabFilter::All) return {};
    TabBarBadge badge;
    badge.active = true;
    badge.label = filter == TabFilter::Group ? "group" : "agents";
    badge.hidden = all.size() >= visible.size() ? all.size() - visible.size() : 0;
    // Hidden ATTENTION: the '!' tabs of `all` that `visible` does not carry.
    // Glyph, not state: Waiting and a plugin raise are the same signal here,
    // exactly as they render as the same '!' on a visible tab.
    for (const auto& tab : all) {
        if (tab.attention_glyph == '\0') continue;
        const bool shown =
            std::any_of(visible.begin(), visible.end(),
                        [&](const auto& v) { return v.tab_id == tab.tab_id; });
        if (!shown) ++badge.attention;
    }
    return badge;
}

ftxui::Element RenderTabBar(const std::vector<AppState::TabSnapshot>& tabs, const Theme& theme,
                            const TabBarBadge& badge, TabHitBoxes* hits) {
    using namespace ftxui;
    if (hits != nullptr) hits->cells.clear();
    if (tabs.empty()) return emptyElement();

    std::vector<std::string> labels;
    std::vector<int> widths;
    labels.reserve(tabs.size());
    widths.reserve(tabs.size());
    // The name cap follows the room there is: a flat 16 columns cut names
    // short on a wide terminal with two tabs. An even share of the width
    // (less the label's own chrome), clamped -- the overflow window below
    // still handles "too many tabs" either way.
    const int per_tab = Terminal::Size().dimx / static_cast<int>(tabs.size()) - 10;
    const auto name_cap = static_cast<std::size_t>(std::clamp(per_tab, 12, 32));
    std::size_t focused_index = 0;
    for (std::size_t i = 0; i < tabs.size(); ++i) {
        auto [label, width] = LabelAndWidth(tabs[i], i + 1, name_cap);
        labels.push_back(std::move(label));
        widths.push_back(width);
        if (tabs[i].is_focused) focused_index = i;
    }

    // "«123 " / " 123»": digits plus the glyph plus a hair of breathing
    // room. Fixed rather than measured per-count -- a count wide enough to
    // overrun this by even one column is a TUI with 1000+ open tabs, an
    // extreme nobody hits in practice, and overrunning a text row by one
    // column is a cosmetic non-event, not a functional break.
    // The filter badge (see the header) rides the bar's trailing edge; its
    // width comes out of the overflow window's budget FIRST, so visible
    // cells + markers + badge always fit together. Two strings: the muted
    // "label ·N" and the attention-colored " !N" suffix, kept separate only
    // for their colors -- the width is the sum.
    std::string badge_text;
    std::string badge_attention;
    int badge_width = 0;
    if (badge.active) {
        badge_text = badge.label;
        if (badge.hidden > 0) badge_text += " ·" + std::to_string(badge.hidden);
        if (badge.attention > 0) badge_attention = " !" + std::to_string(badge.attention);
        badge_width = static_cast<int>(Utf8ColumnWidth(badge_text) +
                                       Utf8ColumnWidth(badge_attention)) +
                      kSeparatorWidth;   // plus the separator that joins it
    }
    const auto window = ComputeTabBarWindow(widths, Terminal::Size().dimx - badge_width,
                                            focused_index, kTabOverflowMarkerWidth);

    Elements cells;
    if (window.hidden_before > 0) {
        cells.push_back(text("«" + std::to_string(window.hidden_before) + " ") | Muted(theme));
    }
    for (std::size_t i = window.visible_start; i < window.visible_end; ++i) {
        if (!cells.empty()) cells.push_back(text(" │ ") | Muted(theme));  // never let labels fuse
        Element cell = RenderTabCell(tabs[i], labels[i], theme);
        if (hits != nullptr) {
            auto box = std::make_shared<Box>();
            cell = std::move(cell) | reflect(*box);
            hits->cells.emplace_back(tabs[i].tab_id, std::move(box));
        }
        cells.push_back(std::move(cell));
    }
    if (window.hidden_after > 0) {
        cells.push_back(text(" " + std::to_string(window.hidden_after) + "»") | Muted(theme));
    }
    if (badge.active) {
        if (!cells.empty()) cells.push_back(text(" │ ") | Muted(theme));
        Element badge_el = text(badge_text) | Muted(theme);
        if (!badge_attention.empty()) {
            badge_el = hbox({std::move(badge_el),
                             text(badge_attention) | color(theme.ColorFor("attention")) | bold});
        }
        cells.push_back(std::move(badge_el));
    }
    return hbox(std::move(cells));
}

}  // namespace minimal::client
