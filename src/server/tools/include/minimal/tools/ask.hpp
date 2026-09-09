#pragma once

// The model→human question primitives, in the tools layer because the only
// consumer is a Tool (the tool-ask plugin's `ask_user`) and the only producer
// of answers is a human at a client.
//
// A THIRD transport shape beside PermissionChecker and commands::
// ConfirmationBroker, and the same rationale applies: a permission request
// carries tool input and a grant model; a command confirmation is a title, a
// detail and flat option strings. A model-initiated question carries several
// questions at once, labeled options with human-facing descriptions, and
// multi-select -- structure the confirmation shape cannot hold without
// bending it (ARCHITECTURE.md "Asking the user").
//
// What all three share is the mechanism underneath: PendingRequests plus the
// session's outbound queue. WireAskBroker (rpc) implements the interface over
// that rendezvous; the tool sees only this header, so it stays testable with
// a lambda for a broker.

#include <string>
#include <vector>

namespace minimal::tools {

struct AskOption {
    std::string label;       // what the model receives back verbatim
    std::string description; // human-facing elaboration; may be empty
};

struct AskQuestion {
    std::string question;
    std::string header; // short human-facing label, e.g. "Approach"; may be empty
    std::vector<AskOption> options;
    bool multiple = false;    // checkbox semantics instead of a single pick
    bool allow_other = true;  // the client may offer a free-text answer
};

struct AskRequest {
    std::vector<AskQuestion> questions;  // the tool validates 1..4
};

struct AskAnswer {
    // One slot per AskQuestion, aligned by index with
    // AskRequest::questions. `selected` holds exact option labels (<= 1
    // unless that question's `multiple` is set); `other` is the free-text
    // answer, empty unless that question's `allow_other` was set. A slot with
    // neither is "question not answered".
    struct Slot {
        std::vector<std::string> selected;
        std::string other;
    };
    std::vector<Slot> slots;
    // False: the questions were never really answered -- dismissed by the
    // user, an unattended timeout elapsed, or the turn was cancelled. The
    // tool maps that onto a plain result, not an error, and never invents
    // selections.
    bool answered = false;
};

class AskBroker {
public:
    virtual ~AskBroker() = default;

    // BLOCKS. Safe only on the session worker thread (or a tool-pool thread
    // in a parallel round) -- the same contract RequestConfirmation has, for
    // the same reason: the call parks on PendingRequests until a client
    // answers, the turn is cancelled, the session terminates, or the
    // unattended timeout elapses. Every one of those is `answered == false`.
    virtual AskAnswer Ask(const AskRequest& request) = 0;
};

}  // namespace minimal::tools
