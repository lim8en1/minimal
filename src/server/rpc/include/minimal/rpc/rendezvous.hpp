#pragma once

// The genuinely-shared skeleton behind WirePermissionChecker::check and
// WireConfirmationBroker::RequestConfirmation, factored out once (Phase 5
// step 10) rather than duplicated: both register with PendingRequests,
// publish their own event, then await, fail closed on no answer (cancelled,
// terminating, or timed out), and fail closed on the wrong PendingAnswer
// alternative -- a client answering against the wrong kind of request id is
// structurally unreadable, not a parse failure the waiter has to detect by
// hand. Deliberately NOT a shared base class: the two brokers implement
// different interfaces (tools::PermissionChecker vs
// commands::ConfirmationBroker, a deliberate port/adapter split -- see
// docs/SERVER_CORE_FIXES.md's "Explicitly not fixing"), and their event
// construction and answer-filtering halves are genuinely different shapes.
// Only the register/await/fail-closed middle is identical; composition
// covers exactly that without forcing either class into a common ancestor.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

#include "minimal/rpc/pending_requests.hpp"

namespace minimal::rpc {

// Awaits `registration`, then narrows the typed PendingAnswer to `Answer`.
// nullopt covers BOTH fail-closed cases (no answer at all, or an answer of
// the other broker's type) -- the caller cannot and need not tell them
// apart, since both mean "no usable answer arrived."
template <typename Answer>
std::optional<Answer> AwaitTypedAnswer(PendingRequests::Registration& registration,
                                       std::chrono::milliseconds unattended_timeout) {
    const std::optional<PendingAnswer> answer = registration.AwaitResponse(
        unattended_timeout > std::chrono::milliseconds::zero()
            ? std::optional(unattended_timeout)
            : std::nullopt);
    if (!answer) return std::nullopt;
    const auto* typed = std::get_if<Answer>(&*answer);
    if (typed == nullptr) return std::nullopt;
    return *typed;
}

// An atomic counter plus a fixed prefix, composed rather than duplicated as
// each class's own next_id_/NextRequestId(). The prefix is a diagnostic
// only -- "perm-" vs "confirm-" in logs -- NOT the safety mechanism; the
// typed PendingAnswer variant AwaitTypedAnswer narrows above is.
class RequestIdGenerator {
public:
    explicit RequestIdGenerator(std::string prefix) : prefix_(std::move(prefix)) {}

    std::string Next() {
        return prefix_ + std::to_string(next_id_.fetch_add(1, std::memory_order_relaxed));
    }

private:
    std::string prefix_;
    std::atomic<std::uint64_t> next_id_{0};
};

}  // namespace minimal::rpc
