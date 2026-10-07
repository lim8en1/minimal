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
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <ftxui/dom/elements.hpp>

#include "minimal/client/plugins/registries.hpp"
#include "minimal/client/render/theme.hpp"

namespace minimal::client::tab_bar {

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

// The bundled tab bar (registries.hpp TabBar): numbered cells with the state
// glyph and color, the '!' attention mark, "↳" for a subagent, the overflow
// window, and the filter badge at the trailing edge. Hidden for a lone tab
// unless a filter is on (it would only repeat the status bar).
class Bar final : public plugins::TabBar {
public:
    ftxui::Element Render(const plugins::TabBarModel& model, const Theme& theme,
                          plugins::TabBarHits& hits) const override;
};

}  // namespace minimal::client::tab_bar
