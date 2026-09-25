#pragma once

// Plugin prompts: a tool asks the human something whose SHAPE belongs to a
// plugin, and blocks on the answer (ARCHITECTURE.md "Plugin prompts").
//
// A THIRD transport shape beside PermissionChecker and commands::
// ConfirmationBroker. Those two are core's own questions (consent with a grant
// model; a command's flat option list). This one is opaque: core carries a
// `kind` and a JSON payload to the client, which routes it to the prompt
// renderer a client plugin registered for that kind, and carries the JSON
// answer back. Core never parses either side -- the tool that asked owns the
// schema and validates the answer against its own request.
//
// What all three share is the mechanism underneath: PendingRequests plus the
// session's outbound queue. WirePluginPromptBroker (rpc) implements the
// interface over that rendezvous; a tool sees only this header, so it stays
// testable with a lambda for a broker.

#include <string>

#include <nlohmann/json.hpp>

namespace minimal::tools {

enum class PromptOutcome {
    Answered,     // `answer` holds the renderer's answer
    Dismissed,    // the user declined to answer
    TimedOut,     // sessions.unattended_prompt_timeout_seconds elapsed
    Cancelled,    // the turn was cancelled or the session is terminating
    Unavailable,  // no human can answer: headless, or no renderer for the kind
};

struct PromptRequest {
    std::string kind;       // e.g. "ask"; picks the client's prompt renderer
    nlohmann::json payload; // the plugin's own request shape
};

struct PromptResult {
    PromptOutcome outcome = PromptOutcome::Unavailable;
    // Parsed but UNVALIDATED: only the asking tool knows what a valid answer
    // to its request looks like. Null unless outcome == Answered.
    nlohmann::json answer = nullptr;
};

class PromptBroker {
public:
    virtual ~PromptBroker() = default;

    // BLOCKS. Safe only on the session worker thread (or a tool-pool thread
    // in a parallel round) -- the same contract RequestConfirmation has, for
    // the same reason: the call parks on PendingRequests until a client
    // answers, the turn is cancelled, the session terminates, or the
    // unattended timeout elapses.
    virtual PromptResult Prompt(const PromptRequest& request) = 0;
};

}  // namespace minimal::tools
