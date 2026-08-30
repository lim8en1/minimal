#pragma once

// Ask the human a question and block until answered.
//
// A TRANSPORT PRIMITIVE, deliberately separate from PermissionChecker even
// though both block on the same rendezvous. A permission request carries
// LLM-originated tool input and a grant model; a command confirmation is a
// title, a detail and a list of options. Forcing one shape to serve both would
// mean a command inventing fake tool input, or the permission prompt losing the
// structure a client renders it from.
//
// What they DO share is the mechanism underneath -- PendingRequests plus the
// session's outbound queue -- and that sharing is the point: this is the one
// place tool and command plumbing intentionally overlap (ARCHITECTURE.md,
// "`/`-command system").

#include <string>
#include <vector>

namespace minimal::commands {

struct ConfirmationRequest {
    // "tool_permission" | "command_confirmation" -- lets a client render the two
    // differently without inspecting the payload.
    std::string kind;
    std::string title;
    std::string detail;
    // The caller's own wording, in the order to display. The first is
    // conventionally the affirmative one, but nothing depends on that: the
    // ANSWER is compared against these strings, so a client cannot invent one.
    std::vector<std::string> options;
};

struct ConfirmationResponse {
    // Empty means no answer -- the turn was cancelled or the session is going
    // away. Callers treat that as "do not proceed"; it is not one of `options`,
    // so a command cannot mistake it for a choice.
    std::string chosen_option;

    bool answered() const { return !chosen_option.empty(); }
};

class ConfirmationBroker {
public:
    virtual ~ConfirmationBroker() = default;

    // BLOCKS. Safe only on the session worker thread -- which is exactly where
    // confirming commands run, because RequiresConfirmation() implies
    // !ReadOnly() and non-read-only commands are queued for the worker.
    virtual ConfirmationResponse RequestConfirmation(const ConfirmationRequest& request) = 0;
};

}  // namespace minimal::commands
