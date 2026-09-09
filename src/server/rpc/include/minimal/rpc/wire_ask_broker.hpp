#pragma once

// tools::AskBroker over the bidi stream.
//
// The ask-side twin of WireConfirmationBroker, built the same way for the
// same reason: REGISTER BEFORE PUBLISHING. The client can answer the instant
// the event is written, so the slot must exist first -- otherwise a fast
// answer arrives with nowhere to go and the worker blocks forever on a
// question that was already answered.
//
// No gRPC types here beyond building the event: this takes the queue and the
// rendezvous, both of which the session owns, so it is testable in-process
// with no channel.

#include <chrono>
#include <string>

#include "minimal/rpc/event_broadcaster.hpp"
#include "minimal/rpc/pending_requests.hpp"
#include "minimal/rpc/rendezvous.hpp"
#include "minimal/tools/ask.hpp"
#include "minimal/v1/session_service.pb.h"

namespace minimal::rpc {

class WireAskBroker : public tools::AskBroker {
public:
    // unattended_timeout: sessions.unattended_prompt_timeout_seconds, see
    // WirePermissionChecker. Zero (the default) waits forever.
    WireAskBroker(std::string session_id, EventBroadcaster& queue, PendingRequests& pending,
                  std::chrono::milliseconds unattended_timeout = {});

    // Blocks the worker until the client answers, the turn is cancelled, the
    // session terminates, or (if configured) the unattended timeout elapses.
    // Every one of those -- and every shape-invalid answer -- is
    // `answered == false`: this broker never invents a selection, the same
    // fail-closed rule RequestConfirmation applies to unoffered options.
    tools::AskAnswer Ask(const tools::AskRequest& request) override;

private:
    std::string session_id_;
    EventBroadcaster& queue_;
    PendingRequests& pending_;
    std::chrono::milliseconds unattended_timeout_;
    // "ask-" keeps ids distinct from the other brokers' prefixes for logs and
    // diagnostics. It is NOT the safety mechanism -- the typed PendingAnswer
    // variant AwaitTypedAnswer narrows against is: an answer of the wrong
    // broker's type delivered against this id is structurally unreadable at
    // the waiter.
    RequestIdGenerator request_ids_{"ask-"};
};

}  // namespace minimal::rpc
