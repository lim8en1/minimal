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
// @-MENTIONS: typing '@' opens the inline suggestion list, queried through the
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

#include <chrono>
#include <cstddef>
#include <functional>
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
    std::function<std::vector<CommandCatalogEntry>()> command_catalog = nullptr);

}  // namespace minimal::client
