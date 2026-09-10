#pragma once

// The prompt editor: a multiline input plus the two live-suggestion surfaces.
//
// NEWLINE: plain Enter SUBMITS; Alt+Enter inserts a newline. Alt+Enter (not
// Shift+Enter) is the default because terminals disagree on what they can
// encode -- Shift+Enter is indistinguishable from Enter without the kitty
// keyboard protocol / CSI-u (ARCHITECTURE.md "Customization surface"). The alt
// sequence is matched as ESC-prefixed CR, its most common encoding; a terminal
// that cannot send it is limited to one-line prompts, and keybindings.json is
// where a user would remap.
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
#include <memory>
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
    std::atomic<bool> query_pending_hint{false};

    // Submitted-line recall (up/down arrow, readline-like), scoped to the
    // whole app rather than per-tab -- one editor Component is shared across
    // tabs, so one history is the simplest model that matches "what did I
    // just type", regardless of which tab it went to.
    std::vector<std::string> history;
    // nullopt = editing fresh content, not browsing. An index into `history`
    // while browsing.
    std::optional<std::size_t> history_cursor;
    // The content that was live when browsing started, restored on arrowing
    // DOWN past the newest history entry.
    std::string history_draft;
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
    // Bracketed-paste (DECSET 2004) in-progress flag: null (the default)
    // makes the factory own a private one, exactly as before. RunTui passes
    // its own so HandleGlobalKey (keybinding_dispatch.cpp) can read the
    // same flag and defer to this editor -- rather than firing a bound
    // action -- while a macOS Option-glyph byte arrives mid-paste (see
    // key_event_parser.cpp's IsOptionGlyphEvent).
    std::shared_ptr<bool> pasting = nullptr);

}  // namespace minimal::client
