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
#include <memory>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ftxui/component/event.hpp>

#include "minimal/client/keybinding_dispatch.hpp"
#include "minimal/client/keybindings.hpp"

using minimal::client::AnyWorkInFlight;
using minimal::client::ApplyTabFilterToggle;
using minimal::client::AppState;
using minimal::client::ClosePluginModal;
using minimal::client::CyclePanelFocus;
using minimal::client::FilterTabs;
using minimal::client::FocusPanelOnClose;
using minimal::client::FocusPanelOnOpen;
using minimal::client::Keybindings;
using minimal::client::NextAttentionTab;
using minimal::client::NextRootGroup;
using minimal::client::NextVisibleTab;
using minimal::client::ResolvedKeymap;
using minimal::client::ResolveGroupRoot;
using minimal::client::SessionStateKind;
using minimal::client::TabFilter;
using minimal::client::TabsToClose;
using minimal::client::TabBusyForInterrupt;

namespace {

AppState::TabSnapshot Tab(std::string id, bool is_subagent = false, int64_t root_ordinal = 0,
                          char attention_glyph = '\0',
                          SessionStateKind state = SessionStateKind::Idle,
                          bool turn_in_flight = false) {
    AppState::TabSnapshot tab;
    // The dispatch functions key on the TAB id (the UI's handle, valid for
    // sessionless tabs too), not the session id.
    tab.tab_id = std::move(id);
    tab.is_subagent = is_subagent;
    tab.root_ordinal = root_ordinal;
    tab.attention_glyph = attention_glyph;
    tab.state = state;
    tab.turn_in_flight = turn_in_flight;
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

// Regression (the ESC-interrupt probe): the busy verdict used to be the
// display state alone. A turn silent between message stops -- the local-model
// providers stretch that window into minutes -- read as Idle, so ESC did
// nothing and ctrl+c hit the idle-quit path and killed the app the user was
// trying to interrupt. turn_in_flight now outranks the display state.
TEST_CASE("TabBusyForInterrupt: turn_in_flight outranks the display state",
          "[keybinding_dispatch][interrupt]") {
    // The busy display states are busy regardless of the flag.
    CHECK(TabBusyForInterrupt(false, SessionStateKind::Thinking));
    CHECK(TabBusyForInterrupt(false, SessionStateKind::Working));
    CHECK(TabBusyForInterrupt(false, SessionStateKind::Waiting));

    // Idle + flag armed: the silent-provider window. The case that killed
    // the old probe.
    CHECK(TabBusyForInterrupt(true, SessionStateKind::Idle));

    // Done/Error display with the flag standing: the ensure-session window
    // (a submit the server has not acked with any wire event yet).
    CHECK(TabBusyForInterrupt(true, SessionStateKind::Done));
    CHECK(TabBusyForInterrupt(true, SessionStateKind::Error));

    // Genuinely idle: nothing to cancel -- ctrl+c may quit.
    CHECK_FALSE(TabBusyForInterrupt(false, SessionStateKind::Idle));
    CHECK_FALSE(TabBusyForInterrupt(false, SessionStateKind::Done));
    CHECK_FALSE(TabBusyForInterrupt(false, SessionStateKind::Error));
}

TEST_CASE("AnyWorkInFlight counts a silent in-flight turn, so closing its tab confirms",
          "[keybinding_dispatch][interrupt]") {
    // Idle display, flag armed: the old check saw nothing in flight and the
    // close (or quit) went through without a confirmation, terminating a
    // live turn.
    const std::vector<AppState::TabSnapshot> tabs = {
        Tab("silent", false, 0, '\0', SessionStateKind::Idle, /*turn_in_flight=*/true),
        Tab("idle", false, 0, '\0', SessionStateKind::Idle),
    };
    CHECK(AnyWorkInFlight(tabs, {"silent"}));
    CHECK_FALSE(AnyWorkInFlight(tabs, {"idle"}));
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

// --- Tab-bar filter (tab.filter_group / tab.filter_agents) -------------------

// Two roots, each with a child -- the shape every filter test below works on.
namespace {
std::vector<AppState::TabSnapshot> TwoGroups() {
    return {
        Tab("root1", /*is_subagent=*/false, /*root_ordinal=*/0),
        Tab("child1", /*is_subagent=*/true, /*root_ordinal=*/0),
        Tab("root2", /*is_subagent=*/false, /*root_ordinal=*/1),
        Tab("child2", /*is_subagent=*/true, /*root_ordinal=*/1),
    };
}
}  // namespace

TEST_CASE("FilterTabs: All returns every tab, Agents keeps roots only",
          "[keybinding_dispatch][tab_filter]") {
    const auto tabs = TwoGroups();

    const auto all = FilterTabs(tabs, TabFilter::All, "root1");
    REQUIRE(all.size() == 4);

    const auto agents = FilterTabs(tabs, TabFilter::Agents, "child1");
    REQUIRE(agents.size() == 2);
    CHECK(agents[0].tab_id == "root1");
    CHECK(agents[1].tab_id == "root2");
}

TEST_CASE("FilterTabs: Group keeps the focused tab's group and contains the focused tab",
          "[keybinding_dispatch][tab_filter]") {
    const auto tabs = TwoGroups();

    // Focused on group 1's child: the whole group 1 survives, group 2 does
    // not, and the focused tab itself is always in view.
    const auto group = FilterTabs(tabs, TabFilter::Group, "child1");
    REQUIRE(group.size() == 2);
    CHECK(group[0].tab_id == "root1");
    CHECK(group[1].tab_id == "child1");

    // Focused on a root: same group, same result.
    const auto from_root = FilterTabs(tabs, TabFilter::Group, "root2");
    REQUIRE(from_root.size() == 2);
    CHECK(from_root[0].tab_id == "root2");
    CHECK(from_root[1].tab_id == "child2");
}

TEST_CASE("FilterTabs: Group on an unknown focused handle falls back to the full list",
          "[keybinding_dispatch][tab_filter]") {
    const auto tabs = TwoGroups();
    // "" on an empty app, or a stale handle after a close: an empty bar
    // reads as "the app lost my tabs", not as a filter.
    CHECK(FilterTabs(tabs, TabFilter::Group, "").size() == 4);
    CHECK(FilterTabs(tabs, TabFilter::Group, "no-such-tab").size() == 4);
}

TEST_CASE("ResolveGroupRoot: a subagent resolves to its root, a root to itself",
          "[keybinding_dispatch][tab_filter]") {
    const auto tabs = TwoGroups();
    CHECK(ResolveGroupRoot(tabs, "child1") == "root1");
    CHECK(ResolveGroupRoot(tabs, "root1") == "root1");
    CHECK(ResolveGroupRoot(tabs, "child2") == "root2");
    CHECK(ResolveGroupRoot(tabs, "no-such-tab").empty());
}

TEST_CASE("NextVisibleTab cycles within the filtered set and wraps",
          "[keybinding_dispatch][tab_filter]") {
    const auto tabs = TwoGroups();

    // Agents filter: only the roots cycle.
    CHECK(NextVisibleTab(tabs, TabFilter::Agents, "root1", /*forward=*/true) == "root2");
    CHECK(NextVisibleTab(tabs, TabFilter::Agents, "root2", true) == "root1");       // wraps
    CHECK(NextVisibleTab(tabs, TabFilter::Agents, "root2", /*forward=*/false) == "root1");

    // Group filter: the cycle stays inside the visible group.
    CHECK(NextVisibleTab(tabs, TabFilter::Group, "root1", true) == "child1");
    CHECK(NextVisibleTab(tabs, TabFilter::Group, "child1", true) == "root1");       // wraps

    // All filter: unchanged cycle over everything.
    CHECK(NextVisibleTab(tabs, TabFilter::All, "child1", true) == "root2");
}

TEST_CASE("NextVisibleTab: a focused tab outside the visible set starts at the edge",
          "[keybinding_dispatch][tab_filter]") {
    const auto tabs = TwoGroups();
    // Defensive (the toggle's refocus should prevent this): forward starts
    // at the first visible tab, backward at the last.
    CHECK(NextVisibleTab(tabs, TabFilter::Agents, "child1", true) == "root1");
    CHECK(NextVisibleTab(tabs, TabFilter::Agents, "child1", false) == "root2");
    CHECK(NextVisibleTab(tabs, TabFilter::Agents, "child1", true) != "child1");

    // Nothing visible at all: empty, never a crash.
    CHECK(NextVisibleTab({}, TabFilter::All, "", true).empty());
}

TEST_CASE("ApplyTabFilterToggle: the active filter toggles back to All",
          "[keybinding_dispatch][tab_filter]") {
    const auto tabs = TwoGroups();
    const auto off = ApplyTabFilterToggle(TabFilter::Group, TabFilter::Group, tabs, "root1");
    CHECK(off.filter == TabFilter::All);
    CHECK(off.focus_tab_id.empty());   // unfiltering hides nothing
}

TEST_CASE("ApplyTabFilterToggle: the other filter replaces the active one",
          "[keybinding_dispatch][tab_filter]") {
    const auto tabs = TwoGroups();
    const auto swapped =
        ApplyTabFilterToggle(TabFilter::Group, TabFilter::Agents, tabs, "root1");
    CHECK(swapped.filter == TabFilter::Agents);
    // "root1" is a root: hidden by nothing, focus stays.
    CHECK(swapped.focus_tab_id.empty());
}

TEST_CASE("ApplyTabFilterToggle: Agents over a focused subagent refocuses the group root",
          "[keybinding_dispatch][tab_filter]") {
    const auto tabs = TwoGroups();
    const auto decision =
        ApplyTabFilterToggle(TabFilter::All, TabFilter::Agents, tabs, "child1");
    CHECK(decision.filter == TabFilter::Agents);
    CHECK(decision.focus_tab_id == "root1");
}

TEST_CASE("ApplyTabFilterToggle: Group over a focused subagent needs no refocus",
          "[keybinding_dispatch][tab_filter]") {
    // Group always contains the focused tab, so focus never has to move.
    const auto tabs = TwoGroups();
    const auto decision = ApplyTabFilterToggle(TabFilter::All, TabFilter::Group, tabs, "child2");
    CHECK(decision.filter == TabFilter::Group);
    CHECK(decision.focus_tab_id.empty());
}

// --- NextPermissionRunMode (permission.cycle) -------------------------------

TEST_CASE("NextPermissionRunMode walks the cycle and treats unknown as normal",
          "[keybinding_dispatch][permission]") {
    using minimal::client::NextPermissionRunMode;
    namespace v1 = minimal::v1;

    CHECK(NextPermissionRunMode("normal") == v1::PERMISSION_RUN_MODE_BYPASS);
    CHECK(NextPermissionRunMode("bypass") == v1::PERMISSION_RUN_MODE_ALLOW_ONLY);
    CHECK(NextPermissionRunMode("allow-only") == v1::PERMISSION_RUN_MODE_NORMAL);
    // Unknown/absent counts as normal -- the server default a fresh session
    // starts on (see the header; the status bar renders absent as nothing).
    CHECK(NextPermissionRunMode("") == v1::PERMISSION_RUN_MODE_BYPASS);
    CHECK(NextPermissionRunMode("garbage") == v1::PERMISSION_RUN_MODE_BYPASS);
}

// --- ResolvedKeymap ---------------------------------------------------------
//
// Built once from a Keybindings document; the point of the parsed form is
// that Matches() is a hash lookup plus a small vector compare, not a
// re-derivation of the whole action->events pipeline per call (see the
// header for the per-event cost this replaces in app.cpp's with_keys).

TEST_CASE("ResolvedKeymap matches an event to the action it's bound to",
         "[keybinding_dispatch][keymap]") {
    const auto keys = Keybindings::Parse(R"({"tab.next": "ctrl+n"})");
    const auto keymap = ResolvedKeymap::Build(keys);

    CHECK(keymap.Matches("tab.next", ftxui::Event::CtrlN));
    CHECK_FALSE(keymap.Matches("tab.next", ftxui::Event::CtrlP));
    // A different action name matches nothing, even for the bound key --
    // Matches() is keyed by ACTION, not by "any binding to this key".
    CHECK_FALSE(keymap.Matches("tab.previous", ftxui::Event::CtrlN));
}

TEST_CASE("ResolvedKeymap: an action never bound matches nothing", "[keybinding_dispatch][keymap]") {
    const auto keys = Keybindings::Parse(R"({"tab.next": "ctrl+n"})");
    const auto keymap = ResolvedKeymap::Build(keys);

    CHECK_FALSE(keymap.Matches("no.such.action", ftxui::Event::CtrlN));
}

TEST_CASE("ResolvedKeymap: an unparseable key spec is reported by Unmapped(), matches nothing",
         "[keybinding_dispatch][keymap]") {
    // "ctrl+alt+tab" has no single well-defined terminal encoding
    // (key_event_parser.cpp's own documented gap) -- ParseKeySpec returns
    // empty for it, which Build() records rather than silently dropping.
    const auto keys = Keybindings::Parse(R"({"weird.action": "ctrl+alt+tab"})");
    const auto keymap = ResolvedKeymap::Build(keys);

    CHECK_FALSE(keymap.Matches("weird.action", ftxui::Event::Tab));
    REQUIRE(keymap.Unmapped().size() == 1);
    CHECK(keymap.Unmapped()[0].first == "weird.action");
    CHECK(keymap.Unmapped()[0].second == "ctrl+alt+tab");
}

TEST_CASE("ResolvedKeymap: a key spec with multiple candidate encodings matches any of them",
         "[keybinding_dispatch][keymap]") {
    // alt+left has two documented candidate encodings (key_event_parser.cpp);
    // Build() must retain both, not just the first.
    const auto keys = Keybindings::Parse(R"({"group.previous": "alt+left"})");
    const auto keymap = ResolvedKeymap::Build(keys);

    CHECK(keymap.Matches("group.previous", ftxui::Event::Special("\x1b[1;3D")));
    CHECK(keymap.Matches("group.previous", ftxui::Event::Special("\x1b\x1b[D")));
}

TEST_CASE("ResolvedKeymap: every core default binds to something", "[keybinding_dispatch][keymap]") {
    // Keybindings::Load's fallback (no keybindings.json) is DefaultBindings();
    // Parse("{}") plus the same defaults reproduces that shape without
    // touching the filesystem. Every core action must round-trip through
    // ParseKeySpec -- an unmapped core default is a bug in the parser or the
    // default table, not a user-config problem.
    auto keys = Keybindings::Parse("{}");
    for (const auto& action : {"tab.next", "tab.previous", "group.previous", "group.next",
                               "jump.attention", "session.picker", "agent.picker",
                               "panel.toggle", "tab.close", "chat.load_older",
                               "chat.scroll_up", "chat.scroll_down", "chat.scroll_bottom",
                               "turn.cancel", "app.quit"}) {
        keys.EnsureDefault(action, keys.KeyFor(action, "unbound-in-test"));
    }
    // EnsureDefault only fills what's unbound, and Parse("{}") binds nothing
    // -- so the loop above is a no-op unless the defaults are supplied
    // directly. Load the real defaults the way Keybindings::Load does when
    // no file exists: via KeyFor's own fallback would just echo back
    // "unbound-in-test". Build the keymap from Keybindings::Load's shape
    // instead by asserting against known default values directly.
    CHECK(ResolvedKeymap::Build(Keybindings::Parse(R"({
        "tab.next": "alt+down", "tab.previous": "alt+up",
        "group.previous": "alt+left", "group.next": "alt+right",
        "tab.filter_group": "alt+f", "tab.filter_agents": "alt+o",
        "jump.attention": "alt+a", "session.picker": "ctrl+o",
        "agent.picker": "alt+g", "panel.toggle": "ctrl+t",
        "panel.focus.next": "ctrl+alt+n", "panel.focus.prev": "ctrl+alt+p",
        "tab.close": "ctrl+w", "chat.load_older": "pageup",
        "chat.scroll_up": "ctrl+alt+k", "chat.scroll_down": "ctrl+alt+j",
        "chat.scroll_bottom": "pagedown",
        "turn.cancel": "escape", "app.quit": "ctrl+q"
    })")).Unmapped().empty());
}

// --- Panel focus rules (keybinding_dispatch.hpp) -----------------------------
//
// PluginViewEntry holds a `const View*`; the tests need only a spec(), so a
// one-stub view stands in (the same shape client_plugins_test.cpp uses).

namespace {

namespace plugins = minimal::client::plugins;

class PanelStubView final : public plugins::View {
public:
    explicit PanelStubView(std::string id) : spec_{std::move(id), "", plugins::ViewPlacement::Panel, ""} {}
    const plugins::ViewSpec& spec() const override { return spec_; }
    ftxui::Component component() override { return nullptr; }

private:
    plugins::ViewSpec spec_;
};

plugins::PluginViewEntry Panel(const std::string& id, bool visible = false) {
    static std::vector<std::unique_ptr<plugins::View>> store;
    store.push_back(std::make_unique<PanelStubView>(id));
    return plugins::PluginViewEntry{store.back().get(), nullptr, visible, false, id, {}};
}

class ModalStubView final : public plugins::View {
public:
    explicit ModalStubView(std::string id)
        : spec_{std::move(id), "", plugins::ViewPlacement::Modal, ""} {}
    const plugins::ViewSpec& spec() const override { return spec_; }
    ftxui::Component component() override { return nullptr; }

private:
    plugins::ViewSpec spec_;
};

// component left null: ClosePluginModal only needs to reassign the slot to
// whatever entry.component is, and null is a perfectly good sentinel for
// checking that the reassignment happened, without this file pulling in a
// real render component (see the file header: no FTXUI event loop here).
plugins::PluginViewEntry ModalEntry(const std::string& id, bool visible = false) {
    static std::vector<std::unique_ptr<plugins::View>> store;
    store.push_back(std::make_unique<ModalStubView>(id));
    return plugins::PluginViewEntry{store.back().get(), nullptr, visible, false, id, {}};
}

}  // namespace

TEST_CASE("FocusPanelOnOpen: the opened panel takes focus, others lose it",
          "[keybinding_dispatch][panel_focus]") {
    std::vector<plugins::PluginViewEntry> views;
    views.push_back(Panel("a"));
    views.push_back(Panel("b"));

    FocusPanelOnOpen(views, views[0]);
    CHECK(views[0].focused);
    CHECK_FALSE(views[1].focused);

    FocusPanelOnOpen(views, views[1]);
    CHECK(views[1].focused);
    CHECK_FALSE(views[0].focused);
}

TEST_CASE("FocusPanelOnClose: closing the focused panel refocuses the first survivor",
          "[keybinding_dispatch][panel_focus]") {
    std::vector<plugins::PluginViewEntry> views;
    views.push_back(Panel("a"));
    views.push_back(Panel("b"));
    views.push_back(Panel("c"));
    views[1].visible = true;
    views[2].visible = true;
    views[2].focused = true;

    FocusPanelOnClose(views, views[2]);
    CHECK_FALSE(views[2].focused);
    CHECK(views[1].focused);  // the first remaining VISIBLE panel

    // Closing an UNFOCUSED panel changes nothing.
    FocusPanelOnClose(views, views[0]);
    CHECK(views[1].focused);
}

TEST_CASE("CyclePanelFocus moves among visible panels and wraps",
          "[keybinding_dispatch][panel_focus]") {
    std::vector<plugins::PluginViewEntry> views;
    views.push_back(Panel("a"));
    views.push_back(Panel("b"));
    views.push_back(Panel("c"));
    views[0].visible = true;
    views[1].visible = true;
    views[2].visible = true;

    // None focused: next lands on the first.
    CyclePanelFocus(views, /*forward=*/true);
    CHECK(views[0].focused);

    // Forward wraps; backward is the inverse direction.
    CyclePanelFocus(views, true);
    CHECK(views[1].focused);
    CyclePanelFocus(views, true);
    CHECK(views[2].focused);
    CyclePanelFocus(views, true);
    CHECK(views[0].focused);
    CyclePanelFocus(views, false);
    CHECK(views[2].focused);

    // Invisible panels never take focus, however they are flagged.
    views[2].visible = false;
    views[1].focused = true;
    CyclePanelFocus(views, true);
    CHECK(views[0].focused);
    CHECK_FALSE(views[2].focused);

    // A single visible panel keeps focus.
    views[0].visible = false;
    CyclePanelFocus(views, true);
    CHECK(views[1].focused);
}

TEST_CASE("CyclePanelFocus with no visible panels is a quiet no-op",
          "[keybinding_dispatch][panel_focus]") {
    std::vector<plugins::PluginViewEntry> views;
    views.push_back(Panel("a"));  // not visible
    CyclePanelFocus(views, true);
    CHECK_FALSE(views[0].focused);
}

// --- ClosePluginModal (keybinding_dispatch.hpp) ------------------------------
//
// FTXUI's Modal() gives the shown modal exclusive input (a Container::Tab
// routes every event to it alone while its show flag is true), so
// HandleGlobalKey's own toggle keybinding -- "main" in Modal()'s terms -- can
// never run again to close a plugin modal once it is up. app.cpp wires
// Escape to this function directly, ahead of forwarding the event to the
// plugin, as a close that never depends on plugin cooperation. These tests
// cover the pure logic; driving an actual Escape keypress through the real
// composed Modal() stack (RunTui itself has no test surface) is not covered
// here.

TEST_CASE("ClosePluginModal closes the shown modal, mirroring the toggle-off exactly, "
          "and leaves panels untouched",
          "[keybinding_dispatch][plugin_modal]") {
    std::vector<plugins::PluginViewEntry> views;
    views.push_back(Panel("panel.a", /*visible=*/true));
    views.push_back(ModalEntry("fixture.modal", /*visible=*/true));

    auto slot = std::make_shared<ftxui::Component>();
    bool show_modal = true;

    CHECK(ClosePluginModal(views, slot, show_modal));
    CHECK_FALSE(views[1].visible);
    CHECK_FALSE(show_modal);
    CHECK(views[0].visible);   // an unrelated visible PANEL is untouched

    // Idempotent: nothing Modal left showing, so a second call is a no-op.
    CHECK_FALSE(ClosePluginModal(views, slot, show_modal));
}

TEST_CASE("ClosePluginModal with nothing shown is a quiet no-op",
          "[keybinding_dispatch][plugin_modal]") {
    std::vector<plugins::PluginViewEntry> views;
    views.push_back(ModalEntry("fixture.modal", /*visible=*/false));
    auto slot = std::make_shared<ftxui::Component>();
    bool show_modal = false;

    CHECK_FALSE(ClosePluginModal(views, slot, show_modal));
    CHECK_FALSE(show_modal);
}

// --- HostUi view requests (ApplyViewRequest) ---------------------------------

TEST_CASE("view requests: show, focus and hide panels; open and close a modal",
          "[keybinding_dispatch][view_requests]") {
    using Op = plugins::ViewRequest::Op;
    std::vector<plugins::PluginViewEntry> views{Panel("p1"), Panel("p2"), ModalEntry("m1")};
    // A modal entry needs a component to open; the stubs return none, so give it one.
    views[2].component = ftxui::Renderer([] { return ftxui::text("m1"); });
    ftxui::Component slot;
    bool show_modal = false;

    CHECK(minimal::client::ApplyViewRequest(views, {Op::kShow, "p1"}, slot, show_modal));
    CHECK(views[0].visible);
    CHECK(views[0].focused);
    CHECK(minimal::client::ApplyViewRequest(views, {Op::kSetPanelVisible, "p2", true}, slot, show_modal));
    CHECK(views[1].focused);
    CHECK(minimal::client::ApplyViewRequest(views, {Op::kFocusPanel, "p1"}, slot, show_modal));
    CHECK(views[0].focused);
    CHECK_FALSE(views[1].focused);
    CHECK(minimal::client::ApplyViewRequest(views, {Op::kSetPanelVisible, "p1", false}, slot, show_modal));
    CHECK_FALSE(views[0].visible);
    CHECK(views[1].focused);   // focus moves to the remaining visible panel

    CHECK(minimal::client::ApplyViewRequest(views, {Op::kShow, "m1"}, slot, show_modal));
    CHECK(show_modal);
    CHECK(slot == views[2].component);
    CHECK(minimal::client::ApplyViewRequest(views, {Op::kClose, "m1"}, slot, show_modal));
    CHECK_FALSE(show_modal);

    // No-ops: unknown id, panel-only ops on a modal, focusing a hidden panel.
    CHECK_FALSE(minimal::client::ApplyViewRequest(views, {Op::kShow, "nope"}, slot, show_modal));
    CHECK_FALSE(minimal::client::ApplyViewRequest(views, {Op::kSetPanelVisible, "m1", true}, slot, show_modal));
    CHECK_FALSE(minimal::client::ApplyViewRequest(views, {Op::kFocusPanel, "p1"}, slot, show_modal));
}

TEST_CASE("view requests: an aliased view answers to its surface, and to its own plugin by id",
          "[keybinding_dispatch][view_requests][names]") {
    // Two plugins' views share the canonical id "files"; client-plugins.json
    // aliased b's to "files2". A surface request (a server view hint) picks by
    // surface; each plugin's OWN request picks its own view by canonical id.
    using Op = plugins::ViewRequest::Op;
    std::vector<plugins::PluginViewEntry> views{Panel("files"), Panel("files")};
    views[0].plugin = "a";
    views[1].plugin = "b";
    views[1].surface_id = "files2";
    ftxui::Component slot;
    bool show_modal = false;

    CHECK(minimal::client::ApplyViewRequest(views, {Op::kShow, "files2"}, slot, show_modal));
    CHECK_FALSE(views[0].visible);
    CHECK(views[1].visible);

    plugins::ViewRequest own{Op::kSetPanelVisible, "files", false, "b"};
    CHECK(minimal::client::ApplyViewRequest(views, own, slot, show_modal));
    CHECK_FALSE(views[1].visible);
    CHECK_FALSE(views[0].visible);  // a's view, same canonical id, untouched

    own.plugin = "a";
    own.visible = true;
    CHECK(minimal::client::ApplyViewRequest(views, own, slot, show_modal));
    CHECK(views[0].visible);
}
