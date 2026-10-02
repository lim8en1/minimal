#pragma once

// Retry timing and the "this will never fit, compact instead" classifier for the
// provider call. The loop decides WHETHER another attempt is wanted; this owns
// HOW LONG to wait and what an error means.

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <random>
#include <utility>

#include "minimal/llm/provider.hpp"

namespace minimal::agent {

class RetryPolicy {
public:
    // `cancelled` is polled while waiting so CancelTurn never sits out a backoff.
    RetryPolicy(std::chrono::milliseconds base_delay, std::chrono::milliseconds max_delay,
                std::function<bool()> cancelled)
        : base_delay_(base_delay), max_delay_(max_delay), cancelled_(std::move(cancelled)) {}

    // Waits before retry `attempt`, polling cancellation. False means "give up"
    // -- cancelled during the wait -- so the caller stops retrying. The server's
    // own hint (a parsed Retry-After) outranks the computed backoff: retrying a
    // 429 sooner than it permitted converts one rate-limit event into
    // max_retries more.
    bool SleepForBackoff(int attempt,
                         std::optional<std::chrono::milliseconds> retry_after = std::nullopt);

    // A provider's "this request does not fit the model's window" refusal. There
    // is no portable error code -- Anthropic, OpenAI and the local servers each
    // phrase it differently -- so this is a message heuristic over a
    // non-retryable 400/413. A false positive costs one compaction attempt; a
    // false negative is the old behaviour (the turn fails).
    static bool IsContextOverflow(const llm::LlmError& error);

private:
    std::chrono::milliseconds base_delay_;
    std::chrono::milliseconds max_delay_;
    std::function<bool()> cancelled_;
    // Backoff jitter only. Owned here rather than shared, so two sessions
    // retrying the same failing backend do not draw the same delays.
    std::mt19937 jitter_{std::random_device{}()};

    bool Cancelled() const { return cancelled_ && cancelled_(); }
};

}  // namespace minimal::agent
