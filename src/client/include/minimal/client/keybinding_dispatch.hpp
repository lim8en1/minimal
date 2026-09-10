#pragma once

// Pure tab-set computations backing the TUI's global keybindings
// (app.cpp "global keybindings"). Extracted specifically because this is
// where TWO real bugs lived, both in exactly this kind of index/scan math:
// the attention-jump scan starting from a fixed index instead of the focused
// tab (so it could jump backwards, or land back on the very tab that was
// Waiting), and the group-navigation cycle needing the same "find my own
// index among a filtered subset" shape. `RunTui` is a single ~1100-line
// function with no test surface reachable through it (three of the TUI's
// worst bugs shipped and went unnoticed for a full milestone specifically
// because nothing there is unit-testable); pulling the PURE parts out into
// functions over `AppState::TabSnapshot` -- a value type, no FTXUI, no
// AppState instance required -- means this class of bug is now a directly
// testable one instead of something only a person at a keyboard finds.
//
// None of this touches AppState or FTXUI: every function here is
// (tabs, ...) -> result, driven entirely by the value snapshot app.cpp
// already takes every frame.

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

// tab.close: the session ids to close for `sid`, in CLOSE order (children
// first, root last -- so a confirmation or a close loop can act on the list
// directly without re-deriving order). Closing a root tab closes its whole
// group; closing a subagent tab closes only itself. Empty if `sid` names no
// known tab.
std::vector<std::string> TabsToClose(const std::vector<AppState::TabSnapshot>& tabs,
                                     const std::string& sid);

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

    bool& show_panel;
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
    // help.overlay: the keymap help overlay. Unlike the pickers above, this
    // one is built once (not staged through a modal host) -- app.cpp's
    // comment on keymap_help_view explains why (no async fetch to rebind
    // for).
    std::function<void()> open_keymap_help;
    std::function<void()> request_quit;         // ctrl+q / idle ctrl+c: app.cpp owns the confirm-if-busy logic
    std::function<void(const std::string&)> close_tab;
    std::function<void(const std::string& session_id, int delta_rows)> scroll_chat;   // ChatView::ScrollBy, if that tab has one
    // chat.scroll_bottom (PageDown): ChatView::ScrollToBottom, if that tab has
    // one. Resets the scroll anchor scrolling up leaves behind -- there was
    // previously no way back to the newest row except scrolling all the way
    // down one row at a time, or losing the scroll position on the next
    // turn.
    std::function<void(const std::string& session_id)> scroll_chat_to_bottom;
    // tab.close, when AnyWorkInFlight over `ids`: app.cpp stages its
    // PendingClose and opens the confirmation modal. Not called when nothing
    // is in flight -- HandleGlobalKey closes those tabs directly via close_tab.
    std::function<void(std::vector<std::string> ids)> confirm_close;

    // chat.row_up/chat.row_down: ChatView::MoveRowFocus, if that tab has one.
    // Same sign convention as scroll_chat (positive = toward older).
    std::function<void(const std::string& session_id, int delta)> move_row_focus;
    // chat.toggle_collapse: ChatView::ToggleCollapseFocused.
    std::function<void(const std::string& session_id)> toggle_collapse_focused;
    // chat.toggle_collapse_all: ChatView::ToggleCollapseAll.
    std::function<void(const std::string& session_id)> toggle_collapse_all;
    // Mouse-click collapse: maps a left-press's screen y onto the focused
    // tab's ChatView (ToggleCollapseAt). True when a collapsible row was
    // under the click and its state was flipped -- the event is consumed
    // ONLY then; a miss (empty space, a non-collapsible row, a sessionless
    // tab) returns false so the click keeps falling through.
    std::function<bool(const std::string& session_id, int screen_y)> toggle_collapse_at;
    // chat.toggle_timestamps: ChatView::ToggleTimestamps.
    std::function<void(const std::string& session_id)> toggle_timestamps;

    // True while a bracketed paste (DECSET 2004) is in progress -- kept live
    // by the prompt editor's own CatchEvent (prompt_editor.cpp), shared here
    // (RunTui passes the same shared_ptr<bool>'s target to both) so
    // HandleGlobalKey can defer to the editor instead of firing a bound
    // action when a macOS Option-glyph byte (key_event_parser.cpp's
    // IsOptionGlyphEvent) arrives mid-paste, byte-identical to a real
    // alt+key press.
    bool& pasting;

    // True while the prompt editor's own @/-suggestion list is open (the
    // single app-wide PromptEditorState's list_open) -- so a busy-tab
    // Escape can defer to the editor's own dismiss-the-list handling
    // instead of reading as turn.cancel first. Null-checked at the call
    // site (every real caller sets it; test fixtures may not need to).
    std::function<bool()> suggestions_open;
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

void FocusPanelOnOpen(std::vector<plugins::PluginViewEntry>& views,
                      plugins::PluginViewEntry& entry);
void FocusPanelOnClose(std::vector<plugins::PluginViewEntry>& views,
                       plugins::PluginViewEntry& entry);
void CyclePanelFocus(std::vector<plugins::PluginViewEntry>& views, bool forward);

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
