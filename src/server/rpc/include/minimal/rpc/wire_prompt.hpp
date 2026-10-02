#pragma once

// The skeleton every "ask the client and block" call shares:
// WirePermissionChecker, WireConfirmationBroker and WirePluginPromptBroker each
// own one
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
#include <algorithm>
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
    // `id_prefix` ("perm-", "confirm-", "prompt-") is a diagnostic for logs only;
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
        return AskDetailed<Answer>(std::forward<Build>(build)).answer;
    }

    // Ask() plus WHY there is no answer, for a caller that reports it (the
    // plugin prompt tells the model "timed out" apart from "cancelled").
    // timed_out is inferred: no answer, the session not shut down, and the
    // configured unattended timeout fully elapsed.
    template <typename Answer>
    struct Detailed {
        std::optional<Answer> answer;
        bool timed_out = false;
    };
    template <typename Answer, typename Build>
    Detailed<Answer> AskDetailed(Build&& build) {
        const std::string request_id =
            id_prefix_ + std::to_string(next_id_.fetch_add(1, std::memory_order_relaxed));
        v1::ServerEvent event;
        event.set_session_id(session_id_);
        std::forward<Build>(build)(event, request_id);
        // Registered WITH the event: the slot keeps it for replay to a stream
        // that attaches while the worker is still parked here.
        auto registration =
            pending_.Register(request_id, event, PendingAnswer{std::in_place_type<Answer>}.index());
        // Never blocks: a full subscriber drops its own oldest. Every attached
        // subscriber gets the prompt; the first answer wins.
        queue_.Publish(std::move(event));

        // THE UNATTENDED BOUND counts only time with NO client attached: a user
        // who is attached but away from the keyboard is exactly who the prompt
        // is waiting for, and auto-denying their work is worse than waiting. The
        // wait runs in slices so attachment can be re-read; a client attaching
        // resets the clock (and, via prompt replay, sees the prompt).
        const bool bounded = unattended_timeout_ > std::chrono::milliseconds::zero();
        std::optional<PendingAnswer> answer;
        bool timed_out = false;
        if (!bounded) {
            answer = registration.AwaitResponse(std::nullopt);
        } else {
            const auto slice = std::min(unattended_timeout_, std::chrono::milliseconds(200));
            std::chrono::milliseconds unattended{0};
            auto last = std::chrono::steady_clock::now();
            while (true) {
                answer = registration.AwaitResponse(slice);
                if (answer || registration.Abandoned()) break;
                const auto now = std::chrono::steady_clock::now();
                if (queue_.subscriber_count() > 0) {
                    unattended = std::chrono::milliseconds::zero();
                } else {
                    unattended += std::chrono::duration_cast<std::chrono::milliseconds>(now - last);
                    if (unattended >= unattended_timeout_) {
                        timed_out = true;
                        break;
                    }
                }
                last = now;
            }
        }
        if (!answer) {
            // The waiter gave up (timeout, cancel): dismiss the modal on every
            // attached client, which would otherwise keep offering a question
            // nobody is waiting on. Skipped at shutdown, when the streams are
            // closing anyway.
            if (!pending_.shut_down()) {
                v1::ServerEvent resolved;
                resolved.set_session_id(session_id_);
                resolved.mutable_prompt_resolved()->set_request_id(request_id);
                queue_.Publish(std::move(resolved));
            }
            return {std::nullopt, timed_out};
        }
        const auto* typed = std::get_if<Answer>(&*answer);
        if (typed == nullptr) return {};
        return {*typed, false};
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
