// Pure-logic tier for the TUI's global-keybinding tab-set computations
// (src/client/keybinding_dispatch.{hpp,cpp}). No AppState, no FTXUI, no
// server -- these are (tabs, ...) -> result functions over the same value
// type app.cpp already snapshots every frame (AppState::TabSnapshot), which
// is the whole point of the extraction: RunTui itself has no test surface,
// but the index/scan math it dispatches through does now.
//
// Two of these cases are regression tests for real bugs: NextAttentionTab
// used to scan from a fixed index instead of the focused tab (it could jump
// backwards, or land back on the tab that was Waiting -- a visible no-op
// exactly when Waiting is the reason someone pressed the key).

#include <algorithm>

#include <catch2/catch_test_macros.hpp>

#include "minimal/client/keybinding_dispatch.hpp"

using minimal::client::AnyWorkInFlight;
using minimal::client::AppState;
using minimal::client::NextAttentionTab;
using minimal::client::NextRootGroup;
using minimal::client::SessionStateKind;
using minimal::client::TabsToClose;

namespace {

AppState::TabSnapshot Tab(std::string id, bool is_subagent = false, int64_t root_ordinal = 0,
                          char attention_glyph = '\0',
                          SessionStateKind state = SessionStateKind::Idle) {
    AppState::TabSnapshot tab;
    tab.session_id = std::move(id);
    tab.is_subagent = is_subagent;
    tab.root_ordinal = root_ordinal;
    tab.attention_glyph = attention_glyph;
    tab.state = state;
    return tab;
}

}  // namespace

TEST_CASE("NextRootGroup cycles among root tabs only, skipping subagents",
         "[keybinding_dispatch]") {
    const std::vector<AppState::TabSnapshot> tabs = {
        Tab("root1", /*is_subagent=*/false, /*root_ordinal=*/0),
        Tab("child1", /*is_subagent=*/true, /*root_ordinal=*/0),
        Tab("root2", /*is_subagent=*/false, /*root_ordinal=*/1),
        Tab("root3", /*is_subagent=*/false, /*root_ordinal=*/2),
    };

    CHECK(NextRootGroup(tabs, "root1", /*forward=*/true) == "root2");
    CHECK(NextRootGroup(tabs, "root2", /*forward=*/true) == "root3");
    CHECK(NextRootGroup(tabs, "root3", /*forward=*/true) == "root1");   // wraps
    CHECK(NextRootGroup(tabs, "root2", /*forward=*/false) == "root1");
    CHECK(NextRootGroup(tabs, "root1", /*forward=*/false) == "root3");   // wraps back
}

TEST_CASE("NextRootGroup with no root tabs returns empty", "[keybinding_dispatch]") {
    CHECK(NextRootGroup({}, "anything", true).empty());
}

TEST_CASE("NextAttentionTab starts scanning AFTER the focused tab, not a fixed index",
         "[keybinding_dispatch][regression]") {
    // Regression for the bug fixed alongside this extraction: the scan used
    // to always start at index 1 regardless of which tab was focused.
    const std::vector<AppState::TabSnapshot> tabs = {
        Tab("a"), Tab("b", false, 0, '!'), Tab("c"), Tab("d", false, 0, '!'),
    };

    // Focused on "c" (index 2): the next Waiting tab going forward is "d",
    // never "b" -- a fixed-index-1 scan would have found "b" instead.
    CHECK(NextAttentionTab(tabs, "c") == "d");
    // Focused on "d" itself: wraps all the way around to "b".
    CHECK(NextAttentionTab(tabs, "d") == "b");
    // Focused on "a": "b" is the next one forward.
    CHECK(NextAttentionTab(tabs, "a") == "b");
}

TEST_CASE("NextAttentionTab wraps to the focused tab itself when it is the only match",
         "[keybinding_dispatch]") {
    // Only ONE tab is Waiting, and it is the focused one: the scan covers
    // every OTHER tab first (none qualify) and wraps back to it last -- a
    // harmless no-op (SetFocusedTab on the already-focused tab), not a
    // failure to find a candidate. What T5 fixed is DIFFERENT: the OLD bug
    // started the scan at a fixed index instead of the focused tab's own, so
    // it could land here (or skip past a real candidate) essentially at
    // random rather than only in this single-candidate case.
    const std::vector<AppState::TabSnapshot> tabs = {
        Tab("a"), Tab("b", false, 0, '!'), Tab("c"),
    };
    CHECK(NextAttentionTab(tabs, "b") == "b");
}

TEST_CASE("NextAttentionTab with no Waiting tabs returns empty", "[keybinding_dispatch]") {
    const std::vector<AppState::TabSnapshot> tabs = {Tab("a"), Tab("b"), Tab("c")};
    CHECK(NextAttentionTab(tabs, "a").empty());
}

TEST_CASE("NextAttentionTab on an empty tab list returns empty without dividing by zero",
         "[keybinding_dispatch]") {
    CHECK(NextAttentionTab({}, "").empty());
}

TEST_CASE("AnyWorkInFlight is true iff a named tab is Thinking, Working, or Waiting",
         "[keybinding_dispatch]") {
    const std::vector<AppState::TabSnapshot> tabs = {
        Tab("idle", false, 0, '\0', SessionStateKind::Idle),
        Tab("thinking", false, 0, '\0', SessionStateKind::Thinking),
        Tab("working", false, 0, '\0', SessionStateKind::Working),
        Tab("waiting", false, 0, '\0', SessionStateKind::Waiting),
        Tab("done", false, 0, '\0', SessionStateKind::Done),
        Tab("error", false, 0, '\0', SessionStateKind::Error),
    };

    CHECK_FALSE(AnyWorkInFlight(tabs, {"idle"}));
    CHECK(AnyWorkInFlight(tabs, {"thinking"}));
    CHECK(AnyWorkInFlight(tabs, {"working"}));
    CHECK(AnyWorkInFlight(tabs, {"waiting"}));
    CHECK_FALSE(AnyWorkInFlight(tabs, {"done"}));
    CHECK_FALSE(AnyWorkInFlight(tabs, {"error"}));
    // Any ONE busy id in the set is enough.
    CHECK(AnyWorkInFlight(tabs, {"idle", "done", "working"}));
    CHECK_FALSE(AnyWorkInFlight(tabs, {}));
    CHECK_FALSE(AnyWorkInFlight(tabs, {"no-such-id"}));
}

TEST_CASE("TabsToClose on a subagent closes only itself", "[keybinding_dispatch]") {
    const std::vector<AppState::TabSnapshot> tabs = {
        Tab("root1", false, 0), Tab("child1", true, 0), Tab("child2", true, 0),
    };
    CHECK(TabsToClose(tabs, "child1") == std::vector<std::string>{"child1"});
}

TEST_CASE("TabsToClose on a root closes its whole group, children first",
         "[keybinding_dispatch]") {
    const std::vector<AppState::TabSnapshot> tabs = {
        Tab("root1", false, 0), Tab("child1", true, 0), Tab("child2", true, 0),
        Tab("root2", false, 1), Tab("child3", true, 1),
    };
    // Order matches iteration order (children interleaved with the root as
    // GetAllTabs orders them) -- this test asserts group MEMBERSHIP, and that
    // root2's group is untouched.
    const auto closed = TabsToClose(tabs, "root1");
    CHECK(closed.size() == 3);
    CHECK(std::find(closed.begin(), closed.end(), "root1") != closed.end());
    CHECK(std::find(closed.begin(), closed.end(), "child1") != closed.end());
    CHECK(std::find(closed.begin(), closed.end(), "child2") != closed.end());
    CHECK(std::find(closed.begin(), closed.end(), "root2") == closed.end());
    CHECK(std::find(closed.begin(), closed.end(), "child3") == closed.end());
}

TEST_CASE("TabsToClose on an unknown session id closes nothing", "[keybinding_dispatch]") {
    const std::vector<AppState::TabSnapshot> tabs = {Tab("root1", false, 0)};
    CHECK(TabsToClose(tabs, "no-such-id").empty());
}
