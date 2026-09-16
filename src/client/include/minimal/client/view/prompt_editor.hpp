#pragma once

// The prompt editor: a multiline input plus the two live-suggestion surfaces.
//
// NEWLINE: plain Enter SUBMITS; Alt+Enter inserts a newline. Alt+Enter (not
// Shift+Enter) is the default because terminals disagree on what they can
// encode -- Shift+Enter is indistinguishable from Enter without the kitty
// keyboard protocol / CSI-u (ARCHITECTURE.md "Customization surface"). The
// alt sequence is matched in every encoding a terminal actually emits: ESC CR
// / ESC LF, the kitty protocol's CSI 13;{mod}u, and xterm modifyOtherKeys'
// CSI 27;{mod};13~ (see IsNewlineEvent). A terminal that reports Option+Enter
// as a PLAIN \r -- the macOS default in Terminal.app, Ghostty, Alacritty and
// WezTerm until option-as-alt / the kitty protocol is enabled -- cannot be
// distinguished from submit by any in-app match; enable the terminal setting
// or remap in keybindings.json.
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
// debounce, the catalog is in memory, fetched once at connect. Prompt
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

#include "minimal/client/render/theme.hpp"
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
    // timeline deliberately did NOT follow: it is app-scoped (PromptHistory
    // below).
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
    // nullopt = editing fresh content, not browsing. An index into `entries`
    // while browsing.
    std::optional<std::size_t> cursor;
    // The content that was live when browsing started, restored on arrowing
    // DOWN past the newest history entry.
    std::string draft;
};

// One /-command the editor may offer. From ListCommands, fetched once at
// connect by the host.
struct CommandCatalogEntry {
    std::string name;
    std::string summary;
    bool read_only = false;
    bool submits_prompt = false;  // PromptShorthand: the one kind that submits a turn
    // Command::RequiresSession: excluded from /-autocomplete (and the /help
    // overlay) while the focused tab has no session -- a command that cannot
    // run without one is never offered as one.
    bool requires_session = true;
};

// True when the command is usable on a tab with NO session: either the
// server marks it requires_session=false, or it is one of the two
// desired-state commands (/agent, /model) whose bare form opens a
// client-owned picker and whose named form sets the tab's desired state --
// neither ever refuses for want of a session (app.cpp's submit_command
// intercepts both before any dispatch). ONE rule gates the /-autocomplete,
// the /help overlay, and the sessionless refusal, so a command offered on a
// sessionless tab is never one that would only refuse there. The names are
// necessarily hardcoded: the interception that makes them sessionless-safe
// is client-side by design (the server's taxonomy is per-command, and these
// two are per-invocation).
inline bool UsableWithoutSession(const CommandCatalogEntry& entry) {
    return !entry.requires_session || entry.name == "agent" || entry.name == "model";
}

// True when the command is usable on a SUBAGENT tab. A child's command
// surface is READ-ONLY server-side (SessionServiceImpl::DispatchCommand
// refuses everything else), so the palette offers only what would not be
// refused there: read-only catalog entries (/cost, /sessions, ...), plus the
// two client-intercepted commands that work on a child (/new opens a tab of
// its own; /detach-session closes the focused child). /agent and /model are
// deliberately NOT usable: their bare forms open pickers and their named
// forms stage desired state, all aimed at the focused tab's session -- and
// choosing what a CHILD runs on is the parent preset's business, not the
// observer's. ONE rule gates the /-autocomplete and the /help overlay, the
// same pattern as UsableWithoutSession above.
inline bool UsableOnSubagent(const CommandCatalogEntry& entry) {
    return entry.read_only || entry.name == "new" || entry.name == "detach-session";
}

// The @-mention query seam (T2: async). `nullopt` means "still waiting --
// call again next frame"; a present (possibly empty) vector is the resolved
// answer. The shipped implementation (RunTui's `suggest` lambda) submits
// SuggestPaths onto AppState's async RPC worker on the FIRST call for a
// given prefix and returns nullopt immediately, then returns the answer once
// the worker publishes it -- so a slow server no longer pauses the frame the
// way a synchronous 250 ms-deadlined call would.
using MentionSuggestor =
    std::function<std::optional<std::vector<std::string>>(const std::string& prefix)>;

// Returns a REFERENCE, not a copy: the catalog is fetched once at connect and
// held for the app's lifetime (see RunTui), and this is invoked on every
// character typed while a line still reads like a `/`-command name -- copying
// the whole catalog vector per keystroke was pure waste for data that never
// changes after startup.
using CommandCatalogSource = std::function<const std::vector<CommandCatalogEntry>&()>;

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
    PromptHistory* history = nullptr);

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

}  // namespace minimal::client
