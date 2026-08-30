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
#include <string>

#include "minimal/commands/confirmation_broker.hpp"
#include "minimal/rpc/event_queue.hpp"
#include "minimal/rpc/pending_requests.hpp"
#include "minimal/v1/session_service.pb.h"

namespace minimal::rpc {

class WireConfirmationBroker : public commands::ConfirmationBroker {
public:
    WireConfirmationBroker(std::string session_id, EventQueue<v1::ServerEvent>& queue,
                           PendingRequests& pending);

    // Blocks the worker until the client answers, the turn is cancelled, or the
    // session terminates. The last two return an empty chosen_option -- "no
    // answer", which the dispatcher maps onto a policy, rather than this
    // inventing one.
    commands::ConfirmationResponse RequestConfirmation(
        const commands::ConfirmationRequest& request) override;

private:
    std::string NextRequestId();

    std::string session_id_;
    EventQueue<v1::ServerEvent>& queue_;
    PendingRequests& pending_;
    // Distinct prefix from WirePermissionChecker's "perm-": both resolve against
    // the SAME PendingRequests, so overlapping ids would let one answer wake the
    // other's waiter.
    std::atomic<std::uint64_t> next_id_{0};
};

}  // namespace minimal::rpc
