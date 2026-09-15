#pragma once

// The tab bar renders IDENTITY and ATTENTION and nothing else (ARCHITECTURE.md
// "Tab states and attention"): the session name (truncated) with the active-tab
// highlight, state carried by COLOR -- which costs zero width -- and the one
// exception, a literal `!` for Waiting, the single state that demands human
// action and must not depend on color alone. No nesting glyphs, no metadata:
// adjacency conveys grouping; the status bar and tools panel carry detail.
//
// OVERFLOW (ARCHITECTURE.md flagged this as undesigned): when every tab's
// cell doesn't fit the terminal width, RenderTabBar shows a contiguous
// window of tabs around the FOCUSED one and a "«N" / "N»" count on
// whichever side has tabs hidden -- never lets the focused tab itself
// scroll out of view, and never silently clips mid-cell.

#include <cstddef>
#include <vector>

#include <ftxui/dom/elements.hpp>

#include "minimal/client/render/theme.hpp"
#include "minimal/client/state.hpp"

namespace minimal::client {

// The contiguous slice of tabs to actually render, plus how many are
// hidden on each side -- the pure decision behind the overflow rule above,
// split out so it is testable without FTXUI (ARCHITECTURE.md "Testing":
// render output itself is not unit-tested, the logic behind it is).
struct TabBarWindow {
    std::size_t visible_start = 0;   // first visible tab index
    std::size_t visible_end = 0;     // one past the last visible tab index
    std::size_t hidden_before = 0;   // == visible_start
    std::size_t hidden_after = 0;    // == cell_widths.size() - visible_end
};

// `cell_widths[i]` is tab i's rendered width (label + glyph + indent, no
// FTXUI involved). Grows a window outward from `focused_index` (clamped
// into range) while it still fits `available_width`, re-checking after
// every step whether a "«N"/"N»" marker (each costing `marker_width`) is
// still needed on either side -- a side stops needing its marker the
// instant the window's edge reaches that end of the tab list, and the
// space that marker would have cost is then free to grow into. If even the
// focused tab plus both markers doesn't fit, degrades to showing just the
// focused tab (still better than an empty bar).
TabBarWindow ComputeTabBarWindow(const std::vector<int>& cell_widths, int available_width,
                                 std::size_t focused_index, int marker_width);

// The filter badge rendered at the bar's trailing edge while a tab filter is
// active (state.hpp's TabFilter): which filter, how many tabs it hides, and
// -- the one number that must never be silent -- how many of those HIDDEN
// tabs carry the attention glyph (Waiting, or a plugin raise). The bar shows
// identity and attention and nothing else; a filter hides identity, so the
// hidden ATTENTION has to surface here or a filtered-out prompt would be
// invisible (jump.attention is the one-press rescue: it scans all tabs and
// clears the filter when its target is hidden -- keybinding_dispatch.cpp).
struct TabBarBadge {
    bool active = false;
    std::string label;          // "group" | "agents"; "" while inactive
    std::size_t hidden = 0;     // tabs the filter keeps out of the bar
    std::size_t attention = 0;  // of those, carrying the '!' glyph
};

// Pure over the two lists (no FTXUI): counts what `visible` filters out of
// `all`, and how many of the hidden carry attention. `filter` only names the
// badge's label; All (no filter) yields an inactive badge. Inactive by
// default-value when the two lists agree.
TabBarBadge ComputeTabBarBadge(const std::vector<AppState::TabSnapshot>& all,
                               const std::vector<AppState::TabSnapshot>& visible,
                               TabFilter filter);

// `badge` (default: inactive) renders nothing -- the plain bar. When active,
// its width is carved out of the overflow window's budget first, so visible
// cells + markers + badge always fit together.
ftxui::Element RenderTabBar(const std::vector<AppState::TabSnapshot>& tabs, const Theme& theme,
                            const TabBarBadge& badge = {});

}  // namespace minimal::client
