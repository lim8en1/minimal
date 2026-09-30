// Pure-logic tier for the tab bar's overflow rule (src/client/view/tab_bar.{hpp,cpp}):
// ComputeTabBarWindow decides which contiguous slice of tabs to show and how
// many are hidden on each side, with no FTXUI involved (ARCHITECTURE.md
// "Testing": render output itself is not unit-tested, the logic behind it is).
// ComputeTabBarBadge -- the tab filter's trailing badge, including the
// hidden-'!' count -- is the same tier: pure over TabSnapshot lists.

#include <catch2/catch_test_macros.hpp>

#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>

#include "minimal/client/render/theme.hpp"
#include "minimal/client/state.hpp"
#include "minimal/client/view/tab_bar.hpp"

using minimal::client::AppState;
using minimal::client::ComputeTabBarBadge;
using minimal::client::ComputeTabBarWindow;
using minimal::client::TabBarBadge;
using minimal::client::TabBarWindow;
using minimal::client::TabFilter;

namespace {

AppState::TabSnapshot FilterTab(std::string id, bool is_subagent, int64_t root_ordinal,
                                char attention_glyph = '\0') {
    AppState::TabSnapshot tab;
    tab.tab_id = std::move(id);
    tab.is_subagent = is_subagent;
    tab.root_ordinal = root_ordinal;
    tab.attention_glyph = attention_glyph;
    return tab;
}

}  // namespace

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
    // hidden counts are ZEROED in the degenerate branch: at this width the
    // markers themselves would eat the one visible cell, and the old true
    // counts made RenderTabBar draw them anyway, contradicting the documented
    // "no markers" degradation.
    CHECK(window.hidden_before == 0);
    CHECK(window.hidden_after == 0);
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

// --- ComputeTabBarBadge (the tab filter's trailing badge) --------------------

TEST_CASE("ComputeTabBarBadge: no filter, no badge", "[tab_bar][tab_filter]") {
    const std::vector<AppState::TabSnapshot> tabs = {FilterTab("root1", false, 0)};
    const auto badge = ComputeTabBarBadge(tabs, tabs, TabFilter::All);
    CHECK_FALSE(badge.active);
}

TEST_CASE("ComputeTabBarBadge: counts what the filter hides, names the filter",
          "[tab_bar][tab_filter]") {
    const std::vector<AppState::TabSnapshot> all = {
        FilterTab("root1", false, 0),
        FilterTab("child1", true, 0),
        FilterTab("root2", false, 1),
        FilterTab("child2", true, 1),
    };

    // Agents hides the two children.
    const auto agents = ComputeTabBarBadge(all, {all[0], all[2]}, TabFilter::Agents);
    CHECK(agents.active);
    CHECK(agents.label == "agents");
    CHECK(agents.hidden == 2);
    CHECK(agents.attention == 0);

    // Group hides the other group entirely.
    const auto group = ComputeTabBarBadge(all, {all[0], all[1]}, TabFilter::Group);
    CHECK(group.active);
    CHECK(group.label == "group");
    CHECK(group.hidden == 2);
}

TEST_CASE("ComputeTabBarBadge: hidden tabs carrying '!' are counted, visible ones are not",
          "[tab_bar][tab_filter]") {
    // child1 (hidden by the Agents filter) waits; root2 (visible) waits too.
    const std::vector<AppState::TabSnapshot> all = {
        FilterTab("root1", false, 0),
        FilterTab("child1", true, 0, '!'),
        FilterTab("root2", false, 1, '!'),
        FilterTab("child2", true, 1),
    };
    const auto badge = ComputeTabBarBadge(all, {all[0], all[2]}, TabFilter::Agents);
    CHECK(badge.hidden == 2);
    // Exactly one hidden '!' (child1): root2's attention is VISIBLE, so it
    // must not inflate the badge -- the badge's only job is the attention
    // the bar can no longer show.
    CHECK(badge.attention == 1);
}

TEST_CASE("ComputeTabBarBadge: identical lists stay inactive regardless of filter",
          "[tab_bar][tab_filter]") {
    // A filter that hides nothing (Agents over an all-roots app) is still
    // labeled -- the user did press a filter key and the bar should say so.
    const std::vector<AppState::TabSnapshot> all = {FilterTab("root1", false, 0),
                                                    FilterTab("root2", false, 1)};
    const auto badge = ComputeTabBarBadge(all, all, TabFilter::Agents);
    CHECK(badge.active);
    CHECK(badge.label == "agents");
    CHECK(badge.hidden == 0);
    CHECK(badge.attention == 0);
}

TEST_CASE("tab bar hit boxes map a click to the tab under it", "[tab_bar]") {
    auto a = FilterTab("tab-a", false, 1);
    a.display_name = "alpha";
    a.is_focused = true;
    auto b = FilterTab("tab-b", false, 2);
    b.display_name = "beta";
    const auto theme = minimal::client::Theme::Parse(R"({"name":"t","colors":{}})",
                                                      minimal::client::ColorDepth::Truecolor);
    minimal::client::TabHitBoxes hits;
    auto bar = minimal::client::RenderTabBar({a, b}, theme, {}, &hits);
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(80), ftxui::Dimension::Fixed(1));
    ftxui::Render(screen, bar);   // the layout pass fills the boxes
    REQUIRE(hits.cells.size() == 2);
    const auto& first = *hits.cells[0].second;
    const auto& second = *hits.cells[1].second;
    CHECK(hits.TabAt(first.x_min, 0) == "tab-a");
    CHECK(hits.TabAt(second.x_max, 0) == "tab-b");
    CHECK(hits.TabAt(79, 0).empty());   // past the last cell
}
