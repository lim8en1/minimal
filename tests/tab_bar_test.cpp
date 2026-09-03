// Pure-logic tier for the tab bar's overflow rule (src/client/view/tab_bar.{hpp,cpp}):
// ComputeTabBarWindow decides which contiguous slice of tabs to show and how
// many are hidden on each side, with no FTXUI involved (ARCHITECTURE.md
// "Testing": render output itself is not unit-tested, the logic behind it is).

#include <catch2/catch_test_macros.hpp>

#include "minimal/client/view/tab_bar.hpp"

using minimal::client::ComputeTabBarWindow;
using minimal::client::TabBarWindow;

TEST_CASE("everything fits: no hidden tabs, no markers reserved", "[tab_bar]") {
    // 5 tabs of width 10 each == 50; well under a generous budget.
    const std::vector<int> widths(5, 10);
    const auto window = ComputeTabBarWindow(widths, /*available_width=*/200,
                                            /*focused_index=*/2, /*marker_width=*/5);
    CHECK(window.visible_start == 0);
    CHECK(window.visible_end == 5);
    CHECK(window.hidden_before == 0);
    CHECK(window.hidden_after == 0);
}

TEST_CASE("overflow grows a window around the focused tab", "[tab_bar]") {
    // 10 tabs of width 10 each == 100. Budget 40 with a 5-wide marker: the
    // focused tab (index 5) plus both markers leaves 40 - 10 - 5 - 5 = 20
    // for growth, i.e. two more tabs total (one on each side, or two on
    // one side) at 10 each.
    const std::vector<int> widths(10, 10);
    const auto window = ComputeTabBarWindow(widths, /*available_width=*/40,
                                            /*focused_index=*/5, /*marker_width=*/5);
    // Focused tab must always be inside the visible window.
    CHECK(window.visible_start <= 5);
    CHECK(window.visible_end > 5);
    // Exactly 2 tabs shown beyond the focused one (3 total), matching the
    // 20-unit growth budget computed above.
    CHECK(window.visible_end - window.visible_start == 3);
    CHECK(window.hidden_before == window.visible_start);
    CHECK(window.hidden_after == 10 - window.visible_end);
    CHECK((window.hidden_before > 0 || window.hidden_after > 0));
}

TEST_CASE("the focused tab is NEVER hidden, even at the very end of the list",
         "[tab_bar]") {
    // Focused tab is the LAST one (index 9): growth can only go left, and
    // the right marker must never appear (there is nothing after index 9
    // to hide).
    const std::vector<int> widths(10, 10);
    const auto window = ComputeTabBarWindow(widths, /*available_width=*/40,
                                            /*focused_index=*/9, /*marker_width=*/5);
    CHECK(window.visible_end == 10);   // includes the focused (last) tab
    CHECK(window.hidden_after == 0);   // nothing after it to hide
    CHECK(window.hidden_before > 0);   // but earlier tabs don't all fit
}

TEST_CASE("a marker's width is reclaimed the instant its side reaches the list edge",
         "[tab_bar]") {
    // 6 tabs of width 10, focused at index 0 (the FIRST tab): there is
    // nothing to hide on the left from the start, so growth should use the
    // reclaimed left-marker width to show MORE tabs on the right than a
    // naive "always reserve both markers" rule would allow.
    // Budget 40, marker 5: focused (10) + right marker (5) = 15 spent
    // immediately; NO left marker needed since start==0 already. 25
    // remaining -> 2 more tabs (20) fit, a 3rd (30 total) does not.
    const std::vector<int> widths(6, 10);
    const auto window = ComputeTabBarWindow(widths, /*available_width=*/40,
                                            /*focused_index=*/0, /*marker_width=*/5);
    CHECK(window.visible_start == 0);
    CHECK(window.hidden_before == 0);
    CHECK(window.visible_end == 3);   // tabs 0,1,2 -- reclaimed width let a 3rd tab in
    CHECK(window.hidden_after == 3);
}

TEST_CASE("even the focused tab plus both markers doesn't fit: degrade to just it, "
         "no markers",
         "[tab_bar]") {
    const std::vector<int> widths(5, 100);   // huge cells
    const auto window = ComputeTabBarWindow(widths, /*available_width=*/20,
                                            /*focused_index=*/2, /*marker_width=*/5);
    CHECK(window.visible_start == 2);
    CHECK(window.visible_end == 3);
    // hidden_before/hidden_after still report the true counts (for a
    // caller that wants them), even though no marker fits to display them.
    CHECK(window.hidden_before == 2);
    CHECK(window.hidden_after == 2);
}

TEST_CASE("an out-of-range focused_index is clamped, not undefined behavior",
         "[tab_bar]") {
    const std::vector<int> widths(3, 10);
    const auto window = ComputeTabBarWindow(widths, /*available_width=*/200,
                                            /*focused_index=*/99, /*marker_width=*/5);
    CHECK(window.visible_start == 0);
    CHECK(window.visible_end == 3);
}

TEST_CASE("an empty tab list produces an empty window", "[tab_bar]") {
    const auto window = ComputeTabBarWindow({}, /*available_width=*/200,
                                            /*focused_index=*/0, /*marker_width=*/5);
    CHECK(window.visible_start == 0);
    CHECK(window.visible_end == 0);
    CHECK(window.hidden_before == 0);
    CHECK(window.hidden_after == 0);
}
