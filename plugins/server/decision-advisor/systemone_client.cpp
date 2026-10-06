#include "systemone_client.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <random>
#include <thread>
#include <utility>

#include "decision.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/common/http_status.hpp"

namespace minimal::decision_advisor {
namespace {

using namespace std::chrono_literals;

// The cancel-poll slice of every backoff sleep -- the longest a cancel waits to
// be noticed. Not a tunable: is_cancelled() is a predicate, so the sleep must
// poll; 25ms is below what a person waiting on a modal perceives, and a
// handful of wakeups per backoff costs nothing next to the HTTP call.
constexpr auto kSleepSlice = 25ms;

// Statuses the loop retries -- HTTP semantics, not a tunable: TypeSafe's own
// SDK policy (408/429/5xx), of which 429 and 529-overloaded are the ones the
// docs actually predict. Minus 501: llama-server's answer for a model that is
// not a decision model, which no retry can change.
bool IsRetryableStatus(int status) {
    return status == common::kHttpRequestTimeout || status == common::kHttpTooManyRequests ||
           (common::IsServerErrorStatus(status) && status != common::kHttpNotImplemented);
}

bool IsRetryableTransport(net::HttpErrorKind kind) {
    switch (kind) {
        case net::HttpErrorKind::ConnectFailed:
        case net::HttpErrorKind::Timeout:
        case net::HttpErrorKind::Other:
            return true;
        case net::HttpErrorKind::Cancelled:
            return false;  // a cancel is terminal everywhere: abstain
        case net::HttpErrorKind::TlsError:
            return false;  // a bad certificate recurs on every attempt
    }
    return false;
}

// Retry-After (seconds) or retry-after-ms (milliseconds), case-insensitively --
// FindHeaderCI exists precisely for callers assembling headers from a response.
std::optional<std::chrono::milliseconds> RetryAfterOf(const net::HttpResponse& response) {
    if (auto seconds = net::FindHeaderCI(response.headers, "Retry-After")) {
        try {
            const double value = std::stod(*seconds);
            if (value >= 0.0) return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::duration<double>(value));
        } catch (...) {
        }  // an HTTP-date or garbage value is treated as absent
    }
    if (auto millis = net::FindHeaderCI(response.headers, "retry-after-ms")) {
        try {
            const double value = std::stod(*millis);
            if (value >= 0.0) return std::chrono::milliseconds(static_cast<int64_t>(value));
        } catch (...) {
        }
    }
    return std::nullopt;
}

// A [0, jitter] fraction of `delay`, subtracted (ParseSettings keeps jitter in
// [0, 1]). One engine per thread: the loop runs a handful of times per Advise,
// so there is no distribution worth sharing state for.
std::chrono::milliseconds ApplyJitter(std::chrono::milliseconds delay, double jitter) {
    if (jitter <= 0.0) return delay;
    static thread_local std::mt19937_64 engine{std::random_device{}()};
    const double fraction = std::uniform_real_distribution<double>(0.0, jitter)(engine);
    const double scaled = static_cast<double>(delay.count()) * (1.0 - fraction);
    return std::chrono::milliseconds(static_cast<int64_t>(std::max(0.0, scaled)));
}

nlohmann::json BuildBody(const Settings& settings, const nlohmann::json& state) {
    // Identical shape for every backend (verified live): {"state", "model",
    // "questions"} with the model id VALUE carrying the backend namespace.
    nlohmann::json questions = settings.questions;
    for (auto it = settings.extra_questions.begin(); it != settings.extra_questions.end(); ++it) {
        questions[it.key()] = it.value();  // fan-out: same request, logged only
    }
    return nlohmann::json{{"state", state}, {"model", settings.model},
                          {"questions", std::move(questions)}};
}

// One bounded sleep: the delay the policy asked for, clamped to the retry cap
// and to the budget remaining, sliced so a cancel never waits out a backoff.
// Returns false when the caller should give up (cancelled or budget gone).
bool SleepFor(std::chrono::milliseconds delay, std::chrono::milliseconds budget_left,
              const std::function<bool()>& is_cancelled) {
    if (budget_left <= 0ms) return false;
    auto remaining = std::min({delay, budget_left});
    if (remaining <= 0ms) return false;
    auto deadline = std::chrono::steady_clock::now() + remaining;
    for (;;) {
        if (is_cancelled()) return false;
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return true;
        const auto slice = std::min(kSleepSlice, std::chrono::duration_cast<std::chrono::milliseconds>(
                                                        deadline - now));
        std::this_thread::sleep_for(slice);
    }
}

// The server's own words for a refusal, when it gave any: {"error":
// {"message": ...}} (llama-server, OpenAI-shape) or {"error": "..."}, else the
// clipped body. llama-server answers a GGUF converted without Clef's decision
// head with 501 "This model is not a decision model" -- the status alone
// would not say which of the operator's knobs is wrong.
std::string ErrorDetailOf(const std::string& body) {
    // Keeps the warn line one line: enough for any server's error sentence,
    // not an HTML error page. Same clip as mcp_connection's stderr quote.
    constexpr std::size_t kMaxDetail = 200;
    auto parsed = nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/false);
    std::string detail;
    if (parsed.is_object()) {
        if (auto error = parsed.find("error"); error != parsed.end()) {
            if (error->is_string()) {
                detail = error->get<std::string>();
            } else if (error->is_object()) {
                if (auto message = error->find("message");
                    message != error->end() && message->is_string()) {
                    detail = message->get<std::string>();
                }
            }
        }
    }
    if (detail.empty()) detail = body;
    if (detail.size() > kMaxDetail) detail = detail.substr(0, kMaxDetail) + "...";
    return detail;
}

}  // namespace

std::chrono::milliseconds BackoffFor(int attempt, const Settings& settings) {
    // Doubles until backoff_max_ms, then stays there: saturating, so no
    // attempt count can overflow and no shift cap is needed.
    std::int64_t delay = settings.backoff_initial_ms;
    for (int i = 0; i < attempt && delay < settings.backoff_max_ms; ++i) delay *= 2;
    return std::chrono::milliseconds(std::min<std::int64_t>(delay, settings.backoff_max_ms));
}

SystemOneOutcome AskSystemOne(const net::HttpClient& http, const Settings& settings,
                              const std::string& api_key, const nlohmann::json& state,
                              std::function<bool()> is_cancelled) {
    SystemOneOutcome outcome;
    if (!is_cancelled) is_cancelled = [] { return false; };

    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::milliseconds(settings.budget_ms);

    net::HttpRequest request;
    request.url = settings.endpoint;
    request.body = BuildBody(settings, state).dump();
    // An empty key sends no header at all -- a local llama-server started
    // without --api-key -- rather than a "Bearer " with nothing after it.
    if (!api_key.empty()) request.headers.push_back({"Authorization", "Bearer " + api_key});
    request.headers.push_back({"Content-Type", "application/json"});
    request.request_timeout = std::chrono::milliseconds(settings.request_timeout_ms);

    const int total_attempts = settings.max_retries + 1;
    for (int attempt = 0; attempt < total_attempts; ++attempt) {
        if (is_cancelled()) {
            outcome.abstain_reason = "cancelled";
            break;
        }
        const auto budget_left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (budget_left <= 0ms) {
            outcome.abstain_reason = "budget exhausted";
            break;
        }

        ++outcome.attempts;
        bool transport_retryable = false;
        std::string transport_error;
        std::optional<int> status;
        std::optional<net::HttpResponse> response;
        try {
            response = http.Post(request);
        } catch (const net::HttpError& e) {
            transport_error = e.what();
            transport_retryable = IsRetryableTransport(e.kind());
        } catch (const std::exception& e) {
            transport_error = e.what();
        }

        if (!response) {
            if (transport_retryable && attempt + 1 < total_attempts) {
                const auto budget_left_now = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now());
                const auto backoff = ApplyJitter(BackoffFor(attempt, settings), settings.jitter);
                common::DefaultLogger().warn(
                    "decision-advisor: attempt {} to {} failed ({}); retrying",
                    attempt + 1, request.url, transport_error);
                if (!SleepFor(backoff, budget_left_now, is_cancelled)) {
                    outcome.abstain_reason =
                        is_cancelled() ? "cancelled" : "budget exhausted";
                    break;
                }
                continue;
            }
            outcome.abstain_reason = transport_error.empty() ? "unknown transport error"
                                                             : transport_error;
            break;
        }

        status = response->status;
        if (common::IsSuccessStatus(response->status)) {
            auto parsed = nlohmann::json::parse(response->body, nullptr,
                                                /*allow_exceptions=*/false);
            if (parsed.is_discarded()) {
                outcome.abstain_reason = "unparseable response body";
                break;
            }
            // A 2xx body that is not the expected envelope is a contract
            // violation: never retried (it would recur), never guessed at.
            auto choice = ExtractActionAnswer(parsed);
            if (choice == std::nullopt) {
                outcome.abstain_reason = "response carries no 'action' choice answer";
                break;
            }
            outcome.ok = true;
            outcome.choice = std::move(choice);
            outcome.answer = parsed.at("answers");
            if (auto model = parsed.find("model"); model != parsed.end() && model->is_string()) {
                outcome.answered_model = model->get<std::string>();
            }
            break;
        }
        if (IsRetryableStatus(*status) && attempt + 1 < total_attempts) {
            const auto budget_left_now = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            auto delay = RetryAfterOf(*response).value_or(
                ApplyJitter(BackoffFor(attempt, settings), settings.jitter));
            // Two clamps, one purpose: no server header and no policy backoff
            // can stretch one Advise past its budget.
            delay = std::min(delay, std::chrono::milliseconds(settings.retry_after_cap_ms));
            common::DefaultLogger().warn("decision-advisor: attempt {} got HTTP {}; retrying",
                                         attempt + 1, *status);
            if (!SleepFor(delay, budget_left_now, is_cancelled)) {
                outcome.abstain_reason = is_cancelled() ? "cancelled" : "budget exhausted";
                break;
            }
            continue;
        }
        // Non-retryable status, or retries spent: fail open with one warn line
        // (401s and 422s recur on every call -- the log line is how the
        // operator finds out the key is wrong or the rubric malformed).
        const std::string detail = ErrorDetailOf(response->body);
        common::DefaultLogger().warn(
            "decision-advisor: giving up after {} attempt(s), HTTP {} from {}{}",
            outcome.attempts, *status, request.url, detail.empty() ? "" : ": " + detail);
        outcome.abstain_reason = "HTTP " + std::to_string(*status);
        if (!detail.empty()) outcome.abstain_reason += " (" + detail + ")";
        break;
    }

    outcome.total = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    if (outcome.attempts == 0) {
        // Cancelled or budget spent before the first attempt fired.
        outcome.total = std::chrono::milliseconds(0);
    }
    if (!outcome.ok && outcome.abstain_reason.empty()) {
        outcome.abstain_reason = "retries exhausted";
    }
    return outcome;
}

}  // namespace minimal::decision_advisor
