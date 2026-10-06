#pragma once

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

namespace minimal::decision_advisor {

// The bundled file every value below comes from: bundled/decision-advisor.json,
// seeded into the config dir at install (never overwritten, user-editable --
// the bundled/ contract). The code carries NO defaults: a value missing from
// both that file and the plugins.json entry is a load error naming the field.
inline constexpr const char* kBundledSettingsFile = "decision-advisor.json";

// One resolved configuration. Built by layering, later wins:
//
//   1. the bundled file's top level        (shared knobs: retry, thresholds, questions)
//   2. bundled "backends"[<backend>]       (a profile: endpoint, model, key, its own thresholds)
//   3. the plugins.json entry's "config"   (the operator's overrides)
//
// "backend" names the profile -- "typesafe", "openrouter", "llama" ship in the
// bundled file, and an operator may add their own there. Every backend speaks
// the same systemone body {"state", "model", "questions"} and envelope
// {"model", "answers", "usage"}; profiles differ only in data. "retry" merges
// field by field; every other key, "questions" included, replaces wholesale.
struct Settings {
    std::string backend;

    // A key reference (config/secrets.hpp: "env:NAME", "secret:NAME", ...),
    // bound at registration: an unset env variable is a startup error, not a
    // silent 401 on the first prompt. Empty = no Authorization header at all
    // (a local llama-server started without --api-key).
    std::string api_key;
    std::string endpoint;
    std::string model;  // the body's "model" value; a router-mode llama-server routes on it

    // Per-ATTEMPT transfer cap, at most budget_ms. Total wall-clock budget
    // for one Advise call, attempts AND backoff sleeps combined --
    // authoritative: when it runs out, abstain. Keep it below the host's
    // permissions.json advisor_timeout_ms, which remains the outer ceiling.
    int request_timeout_ms = 0;
    int budget_ms = 0;

    // Retry policy, the shape of TypeSafe's own SDK RetryPolicy. max_retries
    // counts RETRIES after the first attempt; backoff doubles from
    // backoff_initial_ms up to backoff_max_ms; jitter (0..1) is subtracted
    // from each delay -- 0 makes the loop deterministic, which is what the
    // tests assert against; a Retry-After header is honored up to
    // retry_after_cap_ms so a "Retry-After: 30" can never stretch one Advise
    // past budget_ms.
    int max_retries = 0;
    int backoff_initial_ms = 0;
    int backoff_max_ms = 0;
    double jitter = 0.0;
    int retry_after_cap_ms = 0;

    // The decision rule (decision.hpp): the answer's chosen option must clear
    // an absolute floor AND beat the runner-up by a margin, or the advisor
    // abstains. Calibration is per model -- which is why the profiles carry
    // their own.
    double min_probability_allow = 0.0;
    double min_probability_deny = 0.0;
    double min_margin = 0.0;

    // The questions map sent verbatim; the answer is read from its "action"
    // key, a Choice over {allow, deny, review}.
    nlohmann::json questions = nlohmann::json::object();

    // Merged into the same request (one round trip, the fan-out pattern);
    // answers are logged, never acted on.
    nlohmann::json extra_questions = nlohmann::json::object();
};

// Layers `config` (the plugins.json entry's object; null or empty = none) over
// `bundled` (the parsed bundled file) and validates the result through
// config::Reader. Throws config::ConfigError naming file and path: an unknown
// key at any layer (every profile is checked, not only the selected one), an
// unknown backend, a wrong-typed or out-of-range value, or one no layer sets.
// Never substitutes a value of its own.
Settings ParseSettings(const nlohmann::json& bundled, const nlohmann::json& config);

// Reads the bundled file through config::LoadJsonConfig. Throws
// config::ConfigError when it is missing, unreadable or not JSON.
nlohmann::json LoadBundledSettings(const std::filesystem::path& path);

}  // namespace minimal::decision_advisor
