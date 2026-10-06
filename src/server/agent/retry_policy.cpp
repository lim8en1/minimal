#include "minimal/agent/retry_policy.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <thread>

#include "minimal/common/policy.hpp"
#include "minimal/common/http_status.hpp"

namespace minimal::agent {

bool RetryPolicy::SleepForBackoff(int attempt,
                                  std::optional<std::chrono::milliseconds> retry_after) {
    if (base_delay_.count() <= 0 && !retry_after) return !Cancelled();

    // Exponential with EQUAL jitter: half the ceiling fixed, half random.
    // Jitter matters even for one local user -- several sessions that failed
    // against the same backend would otherwise retry in lockstep forever -- but
    // full jitter (drawing from [0, ceiling]) can legally return zero, which
    // makes "the retry waited" unassertable in a single run and leaves a real
    // 429 answered immediately every so often. Half the ceiling is the floor.
    // `attempt` tracks retries for a configured max_retries that could exceed
    // 31; shifting that far is UB. Anything past 2^30 ms is already far beyond
    // retry_max_delay, so the min() below clamps it regardless.
    const auto ceiling =
        std::min(base_delay_ * (1 << std::min(attempt, 30)), max_delay_);
    const std::int64_t half = ceiling.count() / 2;
    std::uniform_int_distribution<std::int64_t> pick(0, ceiling.count() - half);
    std::chrono::milliseconds delay{half + pick(jitter_)};

    // The server's own hint outranks the computed backoff: retrying a 429
    // sooner than its Retry-After converts one rate-limit event into
    // max_retries more. The hint was parsed, carried and tested end to end --
    // and then ignored here, the only consumer there ever was.
    if (retry_after && *retry_after > delay) delay = *retry_after;

    // Slept in slices, polling cancellation. Sleeping the whole delay would
    // make CancelTurn wait out a backoff the user just asked to abandon --
    // the same argument that put a cancel-polling sink in the compactor.
    for (std::chrono::milliseconds slept{0}; slept < delay; slept += common::kCancelPollSlice) {
        if (Cancelled()) return false;
        std::this_thread::sleep_for(std::min(common::kCancelPollSlice, delay - slept));
    }
    return !Cancelled();
}

// A provider's "this request does not fit the model's window" refusal. There
// is no portable error code -- Anthropic, OpenAI and the local servers each
// phrase it differently -- so this is a message heuristic over a non-retryable
// 400/413. A false positive costs one compaction attempt; a false negative is
// the old behaviour (the turn fails).
bool RetryPolicy::IsContextOverflow(const llm::LlmError& error) {
    if (error.category() != llm::LlmErrorCategory::RequestError) return false;
    if (error.http_status() && *error.http_status() != common::kHttpBadRequest &&
        *error.http_status() != common::kHttpContentTooLarge) {
        return false;
    }
    std::string message = error.what();
    std::transform(message.begin(), message.end(), message.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const char* needle :
         {"context length", "context_length", "context window", "maximum context",
          "prompt is too long", "input is too long", "too many tokens", "exceeds the limit",
          "reduce the length", "request too large", "tokens exceed"}) {
        if (message.find(needle) != std::string::npos) return true;
    }
    return false;
}

}  // namespace minimal::agent
