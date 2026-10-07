#pragma once

// consent-prompts -- the renderers for core's own two prompt kinds:
// "permission" (a tool call that asks: allow once, grant for the session,
// deny, deny with a message, deny and stop) and "confirmation" (a command's,
// or the TUI's own, question with a fixed set of options). Core builds the
// payload from the server's request, keeps the prompt layer above every
// other view, checks the answer and fails closed; how the question looks
// and is answered is this plugin's.
//
// The payloads and answers (JSON):
//   permission:   {"tool_name", "category" (v1::ToolCategory name), "summary",
//                  "complete", "reason", "grants": [{"key", "label"}]}
//              -> {"decision": "allow" | "deny" | "deny_with_message" |
//                  "deny_and_stop", "granted_keys": [...], "message"}
//   confirmation: {"kind", "title", "detail", "options": [...]}
//              -> {"chosen": one of options}

#include <memory>

#include "minimal/client/sdk/views.hpp"
#include "minimal/client/sdk/widgets.hpp"

namespace minimal::client::consent_prompts {

// Both renderers, drawing with `widgets` (the host's kit; host-owned).
std::vector<std::unique_ptr<plugins::PromptRenderer>> MakeRenderers(
    const plugins::WidgetKit* widgets);

}  // namespace minimal::client::consent_prompts
