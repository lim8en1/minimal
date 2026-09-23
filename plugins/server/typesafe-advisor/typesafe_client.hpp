#pragma once

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "decision.hpp"
#include "minimal/net/http_client.hpp"

#include "settings.hpp"

namespace minimal::typesafe_advisor {

// The outcome of one AskJev call, after retries. `answer`/`choice` are set
// exactly when the endpoint answered 2xx with a parseable envelope; every
// other terminal condition -- budget exhausted, retries spent, cancel,
// non-retryable status, unparseable body -- is `abstain` with a reason for
// the log line. There is no error outcome that blocks the caller: a judge
// that cannot answer costs one human prompt, never the turn (the hooks
// fail-open posture).
struct JevOutcome {
    bool ok = false;
    nlohmann::json answer;      // the verbatim answers map (present when ok)
    // The SAME ExtractActionAnswer call that validates the envelope already
    // produces this; carrying it here means the caller (TypesafeAdvisor::
    // Advise) doesn't have to wrap `answer` back into a fake envelope and
    // parse it a second time just to get what this struct already computed.
    std::optional<ChoiceAnswer> choice;
    std::string answered_model;  // the response's model field, when present
    std::string abstain_reason;  // when !ok
    int attempts = 0;
    std::chrono::milliseconds total{0};
};

// Asks Jev one systemone question set about `state` and returns the raw
// envelope (answers["action"] is what ResolveDecision reads).
//
// Retry/budget loop, shaped after TypeSafe's own SDK RetryPolicy (their
// defaults: max_retries 2, backoff 500ms doubling to 5s, jitter 0.25, retry
// 408/429/5xx + connection/timeout errors, honor Retry-After up to a cap):
//
//   retryable     408, 429, 500..599; HttpError{ConnectFailed, Timeout, Other}
//   non-retryable 401, 422, any other status, unparseable body
//   abort         is_cancelled() (checked before every attempt and during
//                 every backoff sleep, in slices) or budget exhausted
//
// `budget` bounds attempts AND backoffs combined and is authoritative; a
// Retry-After header is clamped to settings.retry_after_cap_ms and the sleep
// to the budget remaining, so no server header can stretch one call past the
// budget. `jitter` 0 makes the delays deterministic (tests).
JevOutcome AskJev(const net::HttpClient& http, const Settings& settings,
                  const std::string& api_key, const nlohmann::json& state,
                  std::function<bool()> is_cancelled);

}  // namespace minimal::typesafe_advisor
