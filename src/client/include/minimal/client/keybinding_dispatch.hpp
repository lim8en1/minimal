#pragma once

// Pure tab-set computations backing the TUI's global keybindings
// (app.cpp "global keybindings"). Extracted specifically because this is
// where TWO real bugs lived, both in exactly this kind of index/scan math:
// the attention-jump scan starting from a fixed index instead of the focused
// tab (so it could jump backwards, or land back on the very tab that was
// Waiting), and the group-navigation cycle needing the same "find my own
// index among a filtered subset" shape. `RunTui` (tui_app.cpp) is one long
// function with no test surface reachable through it (three of the TUI's
// worst bugs shipped and went unnoticed for a full milestone specifically
// because nothing there is unit-testable); pulling the PURE parts out into
// functions over `AppState::TabSnapshot` -- a value type, no FTXUI, no
// AppState instance required -- means this class of bug is now a directly
// testable one instead of something only a person at a keyboard finds.
//
// The PURE functions below touch neither AppState nor FTXUI: (tabs, ...) ->
// result, over the value snapshot the frame already takes. The KeyContext /
// HandleGlobalKey half further down is the dispatch those feed, and does.

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>

#include "minimal/client/keybindings.hpp"
#include "minimal/client/plugins/host_seams.hpp"
#include "minimal/client/plugins/registries.hpp"
#include "minimal/client/state.hpp"

namespace minimal::client {

// group.next / group.previous: the next (or previous) ROOT tab to focus,
// cycling among root tabs only (subagent tabs are not group boundaries).
// "" if there are no root tabs. `focused` not matching any root tab
// (including "" or a subagent id) starts the cycle at the first root.
std::string NextRootGroup(const std::vector<AppState::TabSnapshot>& tabs,
                          const std::string& focused, bool forward);

// jump.attention: the next tab carrying the attention glyph ('!'), scanning
// forward from JUST AFTER the focused tab and wrapping across every tab
// (roots and subagents alike -- attention crosses groups). "" if none
// qualify, including when the only qualifying tab is the focused one itself.
std::string NextAttentionTab(const std::vector<AppState::TabSnapshot>& tabs,
                             const std::string& focused);

// True if any of `ids` is Thinking, Working, or Waiting -- the busy states
// that make closing/quitting destructive enough to confirm.
bool AnyWorkInFlight(const std::vector<AppState::TabSnapshot>& tabs,
                    const std::vector<std::string>& ids);

// tab.close: the TAB ids to close for `tab_id`, in CLOSE order (root FIRST,
// children after -- render order; so a confirmation or a close loop can act
// on the list directly without re-deriving order). Closing a root tab closes
// its whole group; closing a subagent tab closes only itself. Empty if
// `tab_id` names no known tab.
std::vector<std::string> TabsToClose(const std::vector<AppState::TabSnapshot>& tabs,
                                     const std::string& tab_id);

// --- Tab-bar filter (tab.filter_group / tab.filter_agents) -------------------
//
// The pure half of the tab-bar filter: which tabs are visible, what
// navigation does among them, and what toggling a filter does to focus.
// All pure over TabSnapshot -- no AppState, no FTXUI (the same tier as
// NextRootGroup above); HandleGlobalKey applies the results through
// SetTabFilter/SetFocusedTab.

// The tabs `filter` keeps, in render order. All: unchanged. Agents: root
// tabs only. Group: the focused tab's group -- its root (matched by
// root_ordinal) plus the children sharing it -- which contains the focused
// tab itself by construction. When `focused` names no tab at all ("" on an
// empty app, a stale handle after a close), Group falls back to the FULL
// list rather than an empty bar: an empty bar reads as "the app lost my
// tabs", not as a filter.
std::vector<AppState::TabSnapshot> FilterTabs(const std::vector<AppState::TabSnapshot>& tabs,
                                              TabFilter filter, const std::string& focused);

// The group's root for `focused`: `focused` itself when it is a root tab,
// otherwise its group's root (the group's first non-subagent tab, by
// root_ordinal -- the root precedes its children in render order). "" when
// `focused` names no tab. This is the refocus target when a filter toggle
// would hide the focused tab.
std::string ResolveGroupRoot(const std::vector<AppState::TabSnapshot>& tabs,
                             const std::string& focused);

// tab.next/previous within the VISIBLE set: cycles the filtered list from
// the focused tab, wrapping. A focused tab outside the list (not expected
// after the toggle's refocus, but defensive) starts at the first (forward)
// or last (backward) visible tab. "" when nothing is visible.
std::string NextVisibleTab(const std::vector<AppState::TabSnapshot>& tabs, TabFilter filter,
                           const std::string& focused, bool forward);

// What toggling `requested` does given the current filter: the ACTIVE filter
// pressed again returns to All; the other one replaces it (they are two
// views of one list, not a stack). `focus_tab_id` is non-empty when the
// focused tab would be hidden by the new filter and focus must move in the
// same press (Agents over a subagent: to its group's root -- the agent that
// owns the child being hidden), so focus is never left on a tab the bar
// does not show. Only Agents can hide the focused tab: Group always
// contains it, All hides nothing.
struct TabFilterDecision {
    TabFilter filter = TabFilter::All;
    std::string focus_tab_id;   // "" = keep the current focus
};
TabFilterDecision ApplyTabFilterToggle(TabFilter current, TabFilter requested,
                                       const std::vector<AppState::TabSnapshot>& tabs,
                                       const std::string& focused);

// The parsed form of a Keybindings document, built ONCE after keybindings.json
// (plus every plugin view's EnsureDefault) has settled, rather than re-parsed
// per event. `with_keys` (app.cpp's global-keybinding CatchEvent) used to do
// `MatchesAny(event, KeyToEvents(keys.KeyFor(action)))` per action per EVENT
// -- ~13 core actions plus one per plugin view, evaluated on every keystroke
// AND every 20 Hz pump tick -- and each KeyFor allocates a std::string,
// each ParseKeySpec lowercases into a new string, splits it into a
// vector<string>, and builds Event objects with their own string payloads.
// A table never changes after startup; re-deriving it per event was pure
// waste for a lookup that reduces to a hash probe plus a linear compare
// against (almost always) one or two candidate Events.
class ResolvedKeymap {
public:
    // Parses every action `keys` binds (core defaults, the user's file, and
    // any plugin-view EnsureDefault entries already folded in by the time
    // this runs -- see app.cpp's load ordering comment). An action whose key
    // spec parses to no known Event is recorded in Unmapped() rather than
    // silently matching nothing forever with no diagnostic.
    static ResolvedKeymap Build(const Keybindings& keys);

    // Whether `event` matches ANY of `action`'s parsed candidate Events. An
    // action Build() never saw (never bound, or bound to an unparseable key)
    // matches nothing -- the same "no mapping" outcome KeyFor's empty
    // fallback produced before.
    bool Matches(std::string_view action, const ftxui::Event& event) const;

    // (action, key) pairs whose key spec parsed to zero candidate Events --
    // the startup-warning condition ParseKeySpec's own header documents
    // ("a name the terminal cannot encode matches nothing"). Named PAIRS,
    // not just actions, so a caller's warning can name the offending key
    // spec too, same as the loop this replaces.
    const std::vector<std::pair<std::string, std::string>>& Unmapped() const {
        return unmapped_;
    }

private:
    // Transparent lookup: Matches takes a string_view and runs ~20 times per
    // key event -- the materialized std::string per probe was allocation
    // churn on the hot input path for zero benefit.
    struct StringViewHash {
        using is_transparent = void;
        std::size_t operator()(std::string_view s) const noexcept {
            return std::hash<std::string_view>{}(s);
        }
    };
    struct StringViewEqual {
        using is_transparent = void;
        bool operator()(std::string_view a, std::string_view b) const noexcept {
            return a == b;
        }
    };
    std::unordered_map<std::string, std::vector<ftxui::Event>, StringViewHash, StringViewEqual>
        table_;
    std::vector<std::pair<std::string, std::string>> unmapped_;
};

// Everything app.cpp's with_keys handler used to close over by `[&]`,
// bundled so the dispatch body itself can live here. Every reference/pointer
// field aliases an object RunTui owns for the whole frame loop's lifetime;
// KeyContext is never stored past one HandleGlobalKey call (constructed
// fresh -- or once and reused -- immediately before it, passed by
// reference), so none of TuiEnvironment's address-stability concerns apply
// here. The few actions that need app.cpp-only state (TabUi's per-tab chat
// scroll, PendingClose's confirmation staging) go through callbacks instead
// of exposing those types here -- keybinding_dispatch stays ignorant of
// both, the same way it already was of AppState's internals.
struct KeyContext {
    AppState& app;
    const ResolvedKeymap& keymap;
    const std::string& focused;   // the focused TAB id (TabMaintenance::Focused), this frame

    bool& show_plugin_modal;

    std::vector<plugins::PluginViewEntry>& plugin_views;
    // The modal layer's plugin-modal slot: `*plugin_modal_child_slot =
    // entry.component()` stages a modal view's component the same way
    // app.cpp's own picker/permission modals reassign their host's slot.
    std::shared_ptr<ftxui::Component>& plugin_modal_child_slot;

    // session.picker/agent.picker OPEN, never toggle-close: while the
    // picker modal is shown, ftxui::Modal() gives it exclusive input --
    // HandleGlobalKey never sees the key again until the modal closes some
    // other way (Escape, resume, delete), so a `show_x = !show_x` toggle
    // here was dead on the close half; only Escape ever actually closed it.
    // PickerController::OpenSessionPicker/OpenAgentPicker already set their
    // own show_x = true, so this needs nothing back from them.
    std::function<void()> open_picker;
    std::function<void()> open_agent_picker;
    // model.picker: previously reachable only via /model -- session.picker
    // and agent.picker each had a default binding, this did not.
    std::function<void()> open_model_picker;
    std::function<void()> request_quit;         // ctrl+q / idle ctrl+c: app.cpp owns the confirm-if-busy logic
    std::function<void(const std::string&)> close_tab;
    // tab.close, when AnyWorkInFlight over `ids`: app.cpp stages its
    // PendingClose and opens the confirmation modal. Not called when nothing
    // is in flight -- HandleGlobalKey closes those tabs directly via close_tab.
    std::function<void(std::vector<std::string> ids)> confirm_close;

    // The chat surface's declared actions (ChatSurface::Actions ids): a key
    // bound to one goes to the focused tab's pane, which does the rest.
    std::vector<std::string> chat_actions;
    std::function<void(const std::string& tab_id, std::string_view action)> chat_action;
    // A mouse event (wheel, click) for the focused tab's pane: true when it
    // was over the chat and meant something there; a miss falls through to
    // the components below (panels, the editor, text selection).
    std::function<bool(const std::string& tab_id, const ftxui::Event& event)> chat_mouse;

    // Whether the focused tab's prompt editor claims `event` ahead of every
    // binding (PromptEditor::Claims: an open suggestion list's keys, a pasted
    // byte that looks like a bound alt+key). Null = it claims nothing.
    std::function<bool(const ftxui::Event&)> editor_claims;

    // The tab whose tab-bar cell is at screen (x, y), "" for none: a left
    // press there focuses it. Null: the tab bar takes no clicks.
    std::function<std::string(int x, int y)> tab_at;
};

// The global-keybinding CatchEvent body (app.cpp's with_keys), as a function
// over KeyContext rather than closed-over locals: RunTui itself has no test
// surface, so this is what makes the dispatch logic reachable from a test
// (mouse wheel handling, the plugin-view panel/toggle precedence, the
// busy-vs-idle branch on ctrl+c/turn.cancel) without driving the real FTXUI
// loop. Returns whether the event was consumed, same as any CatchEvent.
bool HandleGlobalKey(KeyContext& ctx, const ftxui::Event& event);

// --- Panel focus rules (pure over the plugin view table) --------------------
//
// At most ONE visible panel is focused: it alone receives key events and the
// frame draws its chrome (border + title). These three are the only places
// the flag changes -- extracted from the dispatch body so the rules are
// testable directly (AppState's gRPC fixture is far too heavy for what is,
// in the end, a three-line flag dance over a vector).
//
// OPEN: the panel the user just asked for takes focus; every other panel
// loses it (it is the only one that can receive its own keys).
// CLOSE: closing the focused panel moves focus to the first remaining
// visible panel, so ctrl+g never has to "restore" focus from nothing;
// closing an unfocused panel changes nothing.
// CYCLE: next/prev among VISIBLE panels, wrapping; with none focused, focus
// lands on the first visible panel (next) or the last (prev); a single
// visible panel simply keeps focus.

// Tells a view it was just made visible (View::OnShow), containing a throw.
void NotifyShown(const plugins::View& view);

void FocusPanelOnOpen(std::vector<plugins::PluginViewEntry>& views,
                      plugins::PluginViewEntry& entry);
void FocusPanelOnClose(std::vector<plugins::PluginViewEntry>& views,
                       plugins::PluginViewEntry& entry);
void CyclePanelFocus(std::vector<plugins::PluginViewEntry>& views, bool forward);

// Shows one plugin view, for every opener (its keybinding, a command's
// view_hint): a PANEL becomes visible and takes focus if it was hidden; a
// MODAL takes the one modal slot -- every other modal's `visible` retires
// with it (left set, that modal read as shown while it was not, and its own
// key's first press was a dead "toggle off") -- and stays closed if the
// entry has no component. NotifyShown fires either way.
void ShowPluginView(std::vector<plugins::PluginViewEntry>& views,
                    plugins::PluginViewEntry& entry,
                    ftxui::Component& modal_slot, bool& show_modal);

// Applies one queued HostUi view request (host_seams.hpp) on the UI thread,
// through the same helpers the keybinding path uses. Returns whether anything
// changed; an id no entry answers to, or an op that does not fit the view's
// placement, is a no-op.
bool ApplyViewRequest(std::vector<plugins::PluginViewEntry>& views,
                      const plugins::ViewRequest& request, ftxui::Component& modal_slot,
                      bool& show_modal);

// Host-level close for the ONE plugin modal slot, callable from OUTSIDE
// HandleGlobalKey's own dispatch. FTXUI's Modal() gives the shown modal
// exclusive input (see ftxui/component/modal.cpp: a Container::Tab routes
// every event to the modal alone while its show flag is true) -- so once a
// plugin modal is up, HandleGlobalKey's own toggle keybinding (which lives in
// "main", the layer Modal() stops routing to) can never fire again to close
// it. A plugin whose own component neither consumes Escape nor otherwise
// clears the flag traps every key, forever. app.cpp's plugin modal host wires
// Escape here directly, ahead of forwarding the event to the plugin, as a
// close that never depends on plugin cooperation. Finds the currently-shown
// Modal-placement entry (there is at most one, by construction) and closes
// it exactly as the toggle-off would; returns false if none was showing.
bool ClosePluginModal(std::vector<plugins::PluginViewEntry>& views,
                      std::shared_ptr<ftxui::Component>& modal_child_slot, bool& show_modal);

// permission.cycle: the next run mode in the shift+tab cycle. `current` is
// the mode string the tab is showing -- the APPLIED metadata value, or the
// QUEUED desired one when a switch is pending (the applied value only moves
// at the apply-time metadata event, so cycling from it would make two
// mid-turn presses repeat the first transition instead of walking the
// cycle). Unknown/absent strings count as "normal", the server default a
// fresh session starts on: normal -> bypass -> allow-only -> normal.
v1::PermissionRunMode NextPermissionRunMode(const std::string& current);

// The interrupt probe's per-tab busy verdict (turn.cancel / ctrl+c): the
// derived display state PLUS Snapshot::turn_in_flight, which OUTRANKS it.
// MaybeGoIdleLocked resolves the tab to Idle in the windows between message
// stops and the next provider event -- a window the local-model providers
// stretch into minutes (silent prefill, reasoning without deltas) -- while
// the turn is still alive. Trusting the display state alone made ESC dead
// and ctrl+c lethal (idle-quit) exactly there. Pure over one snapshot field
// pair so the verdict is testable without AppState's gRPC fixture.
bool TabBusyForInterrupt(bool turn_in_flight, SessionStateKind state);

}  // namespace minimal::client
