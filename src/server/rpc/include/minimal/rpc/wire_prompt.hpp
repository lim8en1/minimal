#pragma once

// The skeleton every "ask the client and block" call shares:
// WirePermissionChecker, WireConfirmationBroker and WireAskBroker each own one
// WirePrompt and keep only their own event construction and answer filtering.
//
// Ask() registers the request id BEFORE publishing -- the client can answer
// the instant the event is written, so the slot must exist first or a fast
// answer arrives with nowhere to go. It then awaits and narrows the typed
// PendingAnswer to the caller's alternative. nullopt covers every fail-closed
// case alike: cancelled, terminating, timed out, or an answer of another
// broker's type delivered against this id (structurally unreadable here, not
// something a caller has to detect).
//
// Free of gRPC beyond building the event, so it is testable in-process.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>

#include "minimal/rpc/event_broadcaster.hpp"
#include "minimal/rpc/pending_requests.hpp"
#include "minimal/v1/session_service.pb.h"

namespace minimal::rpc {

class WirePrompt {
public:
    // `id_prefix` ("perm-", "confirm-", "ask-") is a diagnostic for logs only;
    // the typed answer narrowing is the safety mechanism. A zero
    // `unattended_timeout` waits forever.
    WirePrompt(std::string session_id, EventBroadcaster& queue, PendingRequests& pending,
               std::chrono::milliseconds unattended_timeout, std::string id_prefix)
        : session_id_(std::move(session_id)),
          queue_(queue),
          pending_(pending),
          unattended_timeout_(unattended_timeout),
          id_prefix_(std::move(id_prefix)) {}

    // `build(event, request_id)` fills in the event's payload; the session id
    // is already set. Blocks until an answer, cancellation, termination or the
    // unattended timeout.
    template <typename Answer, typename Build>
    std::optional<Answer> Ask(Build&& build) {
        const std::string request_id =
            id_prefix_ + std::to_string(next_id_.fetch_add(1, std::memory_order_relaxed));
        auto registration = pending_.Register(request_id);

        v1::ServerEvent event;
        event.set_session_id(session_id_);
        std::forward<Build>(build)(event, request_id);
        // Never blocks: a full subscriber drops its own oldest. Every attached
        // subscriber gets the prompt; the first answer wins.
        queue_.Publish(std::move(event));

        const std::optional<PendingAnswer> answer = registration.AwaitResponse(
            unattended_timeout_ > std::chrono::milliseconds::zero()
                ? std::optional(unattended_timeout_)
                : std::nullopt);
        if (!answer) return std::nullopt;
        const auto* typed = std::get_if<Answer>(&*answer);
        if (typed == nullptr) return std::nullopt;
        return *typed;
    }

private:
    std::string session_id_;
    EventBroadcaster& queue_;
    PendingRequests& pending_;
    std::chrono::milliseconds unattended_timeout_;
    std::string id_prefix_;
    std::atomic<std::uint64_t> next_id_{0};
};

}  // namespace minimal::rpc
