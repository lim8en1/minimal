#pragma once

// ConfirmationBroker over the bidi stream.
//
// The command-side twin of WirePermissionChecker, and built the same way for the
// same reason: REGISTER BEFORE PUBLISHING. The client can answer the instant the
// event is written, so the slot must exist first -- otherwise a fast answer
// arrives with nowhere to go and the worker blocks forever on a question that
// was already answered.
//
// No gRPC types here: this takes the queue and the rendezvous, both of which the
// session owns, so it is testable in-process with no channel.

#include <atomic>
#include <chrono>
#include <string>

#include "minimal/commands/confirmation_broker.hpp"
#include "minimal/rpc/event_broadcaster.hpp"
#include "minimal/rpc/pending_requests.hpp"
#include "minimal/v1/session_service.pb.h"

namespace minimal::rpc {

class WireConfirmationBroker : public commands::ConfirmationBroker {
public:
    // unattended_timeout: sessions.unattended_prompt_timeout_seconds, see
    // WirePermissionChecker. Zero (the default) waits forever.
    WireConfirmationBroker(std::string session_id, EventBroadcaster& queue,
                           PendingRequests& pending,
                           std::chrono::milliseconds unattended_timeout = {});

    // Blocks the worker until the client answers, the turn is cancelled, the
    // session terminates, or (if configured) the unattended timeout elapses.
    // Every one of those returns an empty chosen_option -- "no answer", which
    // the dispatcher maps onto a policy, rather than this inventing one.
    commands::ConfirmationResponse RequestConfirmation(
        const commands::ConfirmationRequest& request) override;

private:
    std::string NextRequestId();

    std::string session_id_;
    EventBroadcaster& queue_;
    PendingRequests& pending_;
    std::chrono::milliseconds unattended_timeout_;
    // "confirm-" keeps ids distinct from WirePermissionChecker's "perm-" for
    // logs and diagnostics. It is NOT the safety mechanism -- the typed
    // PendingAnswer variant is: a permission answer delivered against this id
    // is structurally unreadable at the waiter.
    std::atomic<std::uint64_t> next_id_{0};
};

}  // namespace minimal::rpc
