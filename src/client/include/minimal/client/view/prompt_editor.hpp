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
#include <functional>
#include <string>
#include <vector>

#include <ftxui/component/component.hpp>

#include "minimal/client/render/theme.hpp"
#include "minimal/v1/session_service.pb.h"

namespace minimal::client {

// Owned by RunTui (one per app; the editor targets the FOCUSED tab).
struct PromptEditorState {
    std::string content;                    // the input's text (InputOption::content)
    std::vector<std::string> suggestions;   // live suggestion list (display text)
    std::vector<std::string> completions;   // what Tab inserts (differs from display for commands)
    int suggestion_selected = 0;
    bool list_open = false;
    // Debounce bookkeeping for the SuggestPaths query.
    std::string pending_prefix;
    std::chrono::steady_clock::time_point query_at{};
    bool query_pending = false;
};

// One /-command the editor may offer. From ListCommands, fetched once at
// connect by the host.
struct CommandCatalogEntry {
    std::string name;
    std::string summary;
    bool read_only = false;
    bool submits_prompt = false;  // PromptShorthand: the one kind that submits a turn
};

// The @-mention query seam. The shipped implementation is a synchronous
// SuggestPaths call with a 250 ms deadline (kSuggestPathsDeadline) -- bounded,
// but it still pauses the frame for up to that long. An async implementation
// (query off-loop, results applied on a later frame) is a drop-in replacement
// behind this same signature; the editor never changes.
using MentionSuggestor = std::function<std::vector<std::string>(const std::string& prefix)>;

ftxui::Component MakePromptEditor(
    const Theme& theme, PromptEditorState& state,
    std::function<void(const std::string&)> on_text,
    std::function<void(const std::string&)> on_command,
    const MentionSuggestor& suggest,
    std::function<std::vector<CommandCatalogEntry>()> command_catalog = nullptr);

}  // namespace minimal::client
