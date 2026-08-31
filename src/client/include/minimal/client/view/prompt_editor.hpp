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
// /-COMMANDS: a prompt starting with '/' goes out as a CommandInvocation (E4
// sends and renders it as a notice; display-mode refinement and
// /-autocomplete via ListCommands are E5).

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
    std::vector<std::string> suggestions;   // live @-mention list
    int suggestion_selected = 0;
    bool list_open = false;
    // Debounce bookkeeping for the SuggestPaths query.
    std::string pending_prefix;
    std::chrono::steady_clock::time_point query_at{};
    bool query_pending = false;
};

ftxui::Component MakePromptEditor(
    const Theme& theme, PromptEditorState& state,
    std::function<void(const std::string&)> on_text,
    std::function<void(const std::string&)> on_command,
    std::function<std::vector<std::string>(const std::string& prefix)> suggest);

}  // namespace minimal::client
