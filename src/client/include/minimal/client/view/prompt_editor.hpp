#pragma once

// The prompt editor: a multiline input plus the two live-suggestion surfaces.
//
// NEWLINE: plain Enter SUBMITS; Alt+Enter inserts a newline. Alt+Enter (not
// Shift+Enter) is the default because terminals disagree on what they can
// encode -- Shift+Enter is indistinguishable from Enter without the kitty
// keyboard protocol / CSI-u (ARCHITECTURE.md "Customization surface"). The
// alt sequence is matched in every encoding a terminal actually emits: ESC CR
// / ESC LF, the kitty protocol's CSI 13;{mod}u, and xterm modifyOtherKeys'
// CSI 27;{mod};13~ (see key_event_parser.cpp's enter/return branch). A
// terminal that reports Option+Enter as a PLAIN \r -- the macOS default in
// Terminal.app, iTerm2, Ghostty, Alacritty and WezTerm until option-as-alt /
// the kitty protocol is enabled -- cannot be distinguished from submit by
// any in-app match; enable the terminal setting, or remap the
// "prompt.newline" action in keybindings.json (e.g. to "shift+enter", or to
// any key unrelated to Enter) -- MakePromptEditor's `is_newline_event`
// parameter below is how RunTui wires that binding in; the fallback used
// when it is null is IsNewlineEvent's hardcoded default-key candidate list.
//
// SOFT WRAP: the prompt body is rendered from WrapPromptContent's visual
// lines (word-aware, to the terminal width, at most half the screen tall,
// scrolled to keep the caret visible) instead of FTXUI Input's one-row-per-
// logical-line render, so a long prompt can be reviewed before sending. Only
// the rendering moved: the Input component still owns every key event, and
// click-to-position is re-implemented from the render geometry.
//
// @-MENTIONS: typing '@' arms the inline suggestion list (a bare '@' is a
// live query too, but its empty prefix always resolves to an empty answer,
// so the list opens once a character follows the '@'), queried through the
// SuggestPaths RPC debounced ~100-150 ms (server-resolved, so every frontend
// inherits the strategy). Hand-rolled inline list, NOT ftxui::Dropdown -- that
// is a click-to-open selector; type-ahead reads better inline. Tab completes
// the highlighted suggestion.
//
// /-COMMANDS: a prompt starting with '/' goes out as a CommandInvocation.
// /-AUTOCOMPLETE (E5): while the first token still reads like a command name
// ('/' plus non-space), the local ListCommands catalog filters inline -- no
// debounce, the catalog is an in-memory snapshot (CommandCatalogCache
// refreshes it in the background, per tab scope). Prompt
// shorthands are annotated as the one kind that will submit a turn
// (docs/EXTENSIBILITY.md 4.4), so a macro never surprises anyone.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>

#include "minimal/client/command_catalog.hpp"
#include "minimal/client/render/theme.hpp"
#include "minimal/client/state.hpp"
#include "minimal/v1/session_service.pb.h"

namespace minimal::client {

// Owned by RunTui (one per app; the editor targets the FOCUSED tab).
struct PromptEditorState {
    std::string content;                    // the input's text (InputOption::content)
    // Bound to the Input's InputOption::cursor_position. Only written
    // explicitly by accept_suggestion (prompt_editor.cpp) today -- FTXUI
    // itself only CLAMPS this into content's bounds after an external splice,
    // it never moves it, so a completion that changes content's length
    // without touching this field leaves the cursor mid-word.
    int cursor_position = 0;
    std::vector<std::string> suggestions;   // live suggestion list (display text)
    std::vector<std::string> completions;   // what Tab inserts (differs from display for commands)
    int suggestion_selected = 0;
    bool list_open = false;
    // Debounce bookkeeping for the SuggestPaths query.
    std::string pending_prefix;
    std::chrono::steady_clock::time_point query_at{};
    bool query_pending = false;
    // A CROSS-THREAD-SAFE mirror of query_pending, kept in sync at every site
    // that writes query_pending -- NOT itself authoritative (query_pending
    // is, and is read/written only on the FTXUI loop thread like every other
    // field here). RunTui's redraw pump runs on its own thread and needs to
    // know "is a debounced query armed" to decide whether to force a repaint
    // between UiVersion() changes (the debounce fires from a render pass --
    // see prompt_editor.cpp's render-time gate -- so nothing repaints, the
    // timer never actually elapses). A plain bool read cross-thread here
    // would be a data race (TSan-visible); this field exists solely to make
    // that read well-defined.
    //
    // PER-TAB now that the editor state itself is per-tab -- and the pump
    // must read the FOCUSED tab's copy only: a background tab's debounce
    // cannot fire (it fires from a render pass, and background tabs do not
    // render), so ORing every tab's hint would read "armed" forever once any
    // tab had typed an '@'.
    std::atomic<bool> query_pending_hint{false};
    // PER-TAB note: one PromptEditorState lives in each TabUi (tab_ui.hpp);
    // RunTui's single app-wide instance predates per-tab editors. The recall
    // TIMELINE deliberately did NOT follow: it is app-scoped (PromptHistory
    // below). The browse POSITION into that timeline, and the draft it
    // interrupted, live here instead -- they read/write this state's own
    // `content`, so a position/draft shared across tabs (as PromptHistory
    // once held them) let switching tabs mid-browse clobber another tab's
    // unsaved draft with a stale recall.
    std::optional<std::size_t> history_cursor;
    std::string history_draft;
};

// Submitted-line recall (up/down arrow, readline-like). APP-scoped by design
// -- "what did I just type", regardless of which tab it went to -- unlike the
// per-tab draft/suggestion state above: a timeline is the wrong thing to fork
// per tab, and one shared instance is what keeps up-arrow continuity when a
// user habitually fires the same command across sessions. One PromptHistory
// is created by RunTui and passed to every editor (host-built or standalone);
// a null `history` argument to MakePromptEditor gives that editor a private
// one (every existing test call site), which behaves identically in-editor.
struct PromptHistory {
    void Record(const std::string& line);
    std::vector<std::string> entries;

    // A live browse position (PromptEditorState::history_cursor) registers
    // its address here for as long as its editor is alive, while `entries`
    // is shared across tabs (history != nullptr in MakePromptEditor) -- see
    // Record()'s trim step. Not used by a private/test-owned history (one
    // editor, nothing else can trim out from under its own cursor).
    void RegisterCursor(std::optional<std::size_t>* cursor);
    void UnregisterCursor(std::optional<std::size_t>* cursor);

private:
    std::vector<std::optional<std::size_t>*> cursors_;
};

// The @-mention query seam (T2: async). `nullopt` means "still waiting --
// call again next frame"; a present (possibly empty) vector is the resolved
// answer. The shipped implementation (RunTui's `suggest` lambda) submits
// SuggestPaths onto AppState's async RPC worker on the FIRST call for a
// given prefix and returns nullopt immediately, then returns the answer once
// the worker publishes it -- so a slow server no longer pauses the frame the
// way a synchronous 250 ms-deadlined call would.
using MentionSuggestor =
    std::function<std::optional<std::vector<std::string>>(const std::string& prefix)>;

ftxui::Component MakePromptEditor(
    const Theme& theme, PromptEditorState& state,
    std::function<void(const std::string&)> on_text,
    std::function<void(const std::string&)> on_command,
    const MentionSuggestor& suggest,
    CommandCatalogSource command_catalog = nullptr,
    // Cheap, loop-thread probe for "the focused tab has a session". Catalog
    // entries with requires_session are offered only while this returns
    // true (a sessionless tab never offers a command that cannot run on
    // one). Null = always available.
    std::function<bool()> session_available = nullptr,
    // Cheap, loop-thread probe for "the focused tab is a SUBAGENT". While it
    // returns true, only UsableOnSubagent entries are offered (a child's
    // command surface is read-only server-side). Null = no filter.
    std::function<bool()> subagent_read_only = nullptr,
    // Bracketed-paste (DECSET 2004) in-progress flag: null (the default)
    // makes the factory own a private one, exactly as before. RunTui passes
    // its own so HandleGlobalKey (keybinding_dispatch.cpp) can read the
    // same flag and defer to this editor -- rather than firing a bound
    // action -- while a macOS Option-glyph byte arrives mid-paste (see
    // key_event_parser.cpp's IsOptionGlyphEvent).
    std::shared_ptr<bool> pasting = nullptr,
    // Submitted-line recall (up/down arrow). Null (the default) gives the
    // editor a PRIVATE one -- same behavior, no cross-editor sharing -- so
    // every pre-existing call site is unchanged. RunTui passes ONE shared
    // instance to every tab's editor: recall is app-scoped by design (see
    // PromptHistory).
    PromptHistory* history = nullptr,
    // "prompt.newline" (keybindings.json), pre-resolved: null (the default,
    // every pre-existing call site) falls back to IsNewlineEvent's hardcoded
    // six-candidate default-key list, unchanged from before this parameter
    // existed. RunTui passes `[&keymap](event){ return
    // keymap.Matches("prompt.newline", event); }` so a user's rebind (see
    // the NEWLINE comment above) actually takes effect.
    std::function<bool(const ftxui::Event&)> is_newline_event = nullptr);

// THE PER-TAB PROMPT EDITOR. A stable outer component in the FTXUI tree that
// owns one PromptEditorState + one editor Component PER TAB, built lazily on
// first use and swapped in at Render()/OnEvent() time by resolving the
// focused tab LIVE -- the MakeModalHost mechanics (a stable shell whose
// inner slot swaps per request) applied to the editor, for the same reason:
// FTXUI's InputOption binds `content`/`cursor_position` by POINTER at
// construction, so one shared editor can never follow tab switches, and the
// draft/mention state must die with the tab (TabUi) the way every other
// per-tab UI cache does.
//
// The states/editors live in PerTabPromptEditors (below) rather than inside
// the host component because three consumers resolve the CURRENT tab's state
// from OUTSIDE the component tree: KeyContext::suggestions_open (busy-tab
// Escape deferral), the /help overlay's insert-into-prompt callback, and
// RunTui's redraw pump (query_pending_hint, read from the pump THREAD --
// hence the mutex). The host references the same store; Render/OnEvent and
// the outer readers all key by the same resolution rule.

// Owns one PromptEditorState + one editor Component per tab. Loop-thread for
// everything except QueryPendingHint (the pump; the mutex exists for it).
// A missing entry for a tab the host resolves CREATES it lazily -- a tab
// that never showed a prompt never pays for one.
class PerTabPromptEditors {
public:
    // The editor for `tab`, built by `build` on first resolution. An empty
    // `tab` is the PRE-TAB editor (the frame before any tab exists): typing
    // there goes through the same submit paths, which open a tab on submit
    // (app.cpp's focused_or_new_tab) -- exactly the old single-editor
    // behavior. Loop-thread only (build may capture caller locals).
    ftxui::Component Resolve(
        const std::string& tab,
        const std::function<ftxui::Component(PromptEditorState&)>& build);

    // The state behind `tab` (same keying as Resolve), created if missing.
    // Loop-thread only: the /help overlay's insert and suggestions_open use
    // it; the reference stays valid until ForgetTab erases that tab.
    PromptEditorState& CurrentState(const std::string& tab);

    // The pump thread's read of the focused tab's debounce hint. find() only
    // (no insert -- a tab the pump peeked at needs no state), false when
    // absent. Safe against concurrent Resolve/ForgetTab.
    bool QueryPendingHint(const std::string& tab) const;

    // Tab close: drops the tab's state AND editor together (the editor's
    // closures capture the state's address; neither may outlive the other).
    // The "" pre-tab entry is never forgotten.
    void ForgetTab(const std::string& tab);

private:
    // Guards both maps (structure AND node lifetime). Everything on the FTXUI
    // loop thread takes it briefly to resolve; the pump takes it to read one
    // atomic. State FIELD writes stay lock-free after resolution: they are
    // loop-thread-only (prompt_editor.cpp), and the one cross-thread field,
    // query_pending_hint, is itself atomic.
    mutable std::mutex mtx_;
    std::map<std::string, PromptEditorState> states_;
    std::map<std::string, ftxui::Component> editors_;

    // The host's Focusable() override peeks at editors_ without going through
    // Resolve (a peek must not build). Same module, one line, kept deliberate.
    friend class PerTabEditorHost;
};

ftxui::Component MakePerTabPromptEditor(PerTabPromptEditors& tabs,
                                        std::function<std::string()> focused_tab,
                                        std::function<ftxui::Component(PromptEditorState&)> build_editor);

// Per-tab in-flight slots for the @-mention suggest RPC. THE REGRESSION IT
// FIXES: the suggestor used ONE shared MentionQuery struct for every tab
// (app.cpp), so a second tab's query -- or the same tab re-firing after a
// refocus -- overwrote the slot before the first answer was consumed, and the
// completion callback's staleness guard then DROPPED that answer: the first
// tab's list never filled ("@ suggestions stopped working", intermittently --
// it worked exactly as long as no second query superseded the first).
//
// One slot per tab, keyed by the same tab id the scope string embeds. The
// contract is the MentionSuggestor's (above): nullopt = still waiting, poll
// again next frame; a present (possibly empty) vector = the resolved answer,
// consumed once.
//
// Cross-thread by design: the FTXUI loop thread calls Poll (from the render
// gate), AppState's RPC worker calls Complete (the RequestSuggestPaths
// callback); one mutex guards the map and every slot's fields. Loop
// re-entrancy note: Complete runs on the worker with NO loop-side lock held
// (Poll returns before the RPC is submitted), so the lock order is trivially
// acyclic.
class MentionQuerySlots {
public:
    // The (prefix-keyed) query slot for `tab`. First call for a (tab, prefix)
    // arms the slot (in_flight=true) and returns nullopt; later calls with
    // the SAME prefix return nullopt while the answer is in flight, then
    // consume and return the answer once Complete(tab, same prefix) ran. A
    // DIFFERENT prefix supersedes the slot's in-flight bookkeeping (its late
    // answer is dropped by Complete's guard) and arms anew.
    std::optional<std::vector<std::string>> Poll(const std::string& tab,
                                                 const std::string& prefix);

    // The RPC worker's answer for `tab`'s most recent query: recorded only
    // when the slot's prefix still matches (a stale answer for a superseded
    // query is dropped). Empty on the NEXT Poll after a match.
    void Complete(const std::string& tab, const std::string& prefix,
                  std::vector<std::string> paths);

    // Drops `tab`'s slot, if any. Called from tab close (app.cpp's
    // close_tab) so a closed tab's in-flight query bookkeeping does not
    // outlive it -- the same per-tab-cache cleanup PerTabPromptEditors::
    // ForgetTab does for the editor state.
    void Forget(const std::string& tab);

private:
    struct Slot {
        std::string prefix;
        bool in_flight = false;
        bool ready = false;
        std::vector<std::string> result;
    };

    mutable std::mutex mtx_;
    std::map<std::string, Slot> slots_;
};

// The TUI's MentionSuggestor: per-tab slots (above) in front of AppState's
// async SuggestPaths RPCs. Extracted from RunTui's local `suggest` lambda so
// the slot mechanics are unit-testable without a running app loop, and so the
// sessioned/sessionless scoping has ONE home (app.cpp builds it with the
// probes it already owns; see the definition for the seam contract).
MentionSuggestor MakeMentionSuggestor(
    AppState* app, MentionQuerySlots& slots,
    std::function<std::string(const std::string& session_id)> session_root_fallback);

}  // namespace minimal::client
