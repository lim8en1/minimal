#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace minimal::typesafe_advisor {

// Which Jev surface the advisor calls. Both carry the systemone body
// {"state", "model", "questions"} -- verified live 2026-01-14 -- so the
// backend changes the endpoint, the key-env default and the model-id VALUE,
// never the request or response shape.
enum class Backend {
    Typesafe,    // POST <endpoint>, default model "jev-latest"
    OpenRouter,  // POST https://openrouter.ai/api/alpha/decisions, "~typesafe/jev-latest"
};

// This plugin's own config, the "config" object on its plugins.json entry.
// Same rules as every bundled plugin's settings: every field optional, missing
// or wrong-typed values keep the struct's defaults.
struct Settings {
    Backend backend = Backend::Typesafe;

    // Resolved from the environment ONCE at registration (the ResolveApiKey
    // convention): unset is a startup error, not a silent 401 on the first
    // prompt. Overridden per backend when the entry does not name one.
    std::string api_key_env = "TYPESAFE_API_KEY";
    std::string endpoint;  // empty = the backend's default URL
    std::string model;     // empty = the backend's default model id

    // Per-ATTEMPT transfer cap. The healthy call is 0.1-0.5s; a cap aborts a
    // wedged attempt instead of letting it eat the whole budget.
    int request_timeout_ms = 2000;
    // Total wall-clock budget for one Advise call: attempts AND backoff sleeps
    // combined. Authoritative -- when it runs out, abstain. Keep it below the
    // host's permissions.json advisor_timeout_ms (default 30s), which remains
    // the outer ceiling.
    int budget_ms = 2000;

    // Retry policy, the shape of TypeSafe's own SDK RetryPolicy scaled down
    // for a permission prompt. max_retries counts RETRIES after the first
    // attempt; backoff doubles from backoff_initial_ms up to backoff_max_ms;
    // jitter (0..1) is subtracted from each delay -- 0 makes the loop
    // deterministic, which is what the tests assert against; a Retry-After
    // header is honored up to retry_after_cap_ms so a "Retry-After: 30" can
    // never stretch one Advise past budget_ms.
    int max_retries = 2;
    int backoff_initial_ms = 100;
    int backoff_max_ms = 2000;
    double jitter = 0.25;
    int retry_after_cap_ms = 1000;

    // The decision rule (decision.hpp): the answer's chosen option must clear
    // an absolute floor AND beat the runner-up by a margin, or the advisor
    // abstains. Deny is gated stricter than allow by default: a wrong
    // auto-allow hands the model a capability, a wrong auto-deny merely sends
    // the model a refusal it can act on.
    double min_probability_allow = 0.75;
    double min_probability_deny = 0.85;
    double min_margin = 0.25;

    // The questions map sent verbatim. Empty = the built-in action question
    // (decision.hpp). An operator may override it wholesale to change the
    // rubric; the answer is still read from the "action" key.
    nlohmann::json questions = nlohmann::json::object();

    // Merged into the same request (one round trip, the fan-out pattern);
    // answers are logged, never acted on.
    nlohmann::json extra_questions = nlohmann::json::object();

    // Endpoint + model with the backend default filled in.
    std::string EffectiveEndpoint() const;
    std::string EffectiveModel() const;
};

// Throws std::runtime_error when api_key_env names an environment variable
// that is unset -- the same fail-loud-at-startup posture providers.json's
// ResolveApiKey takes, and for the same reason: a silently empty key fails
// later as a cryptic 401 on the first gated tool call instead of a clear
// startup error. Never throws for merely-unset optional config.
Settings ParseSettings(const nlohmann::json& config);

}  // namespace minimal::typesafe_advisor
