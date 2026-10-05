#include "settings.hpp"

#include <cstdlib>
#include <stdexcept>

#include "minimal/config/parse_util.hpp"

namespace minimal::typesafe_advisor {
namespace {

constexpr const char* kTypesafeEndpoint = "https://api.typesafe.ai/v1/systemone";
constexpr const char* kTypesafeModel = "jev-latest";
constexpr const char* kOpenRouterEndpoint = "https://openrouter.ai/api/alpha/decisions";
constexpr const char* kOpenRouterModel = "~typesafe/jev-latest";

template <typename T>
T ValueOr(const nlohmann::json& parent, const char* key, T fallback) {
    auto it = parent.find(key);
    if (it == parent.end() || it->is_null()) return fallback;
    if constexpr (std::is_same_v<T, std::string>) {
        if (!it->is_string()) return fallback;
    } else if constexpr (std::is_same_v<T, double>) {
        if (!it->is_number()) return fallback;
    } else {
        if (!it->is_number()) return fallback;
    }
    return it->get<T>();
}

// Nested "retry" object.
int RetryIntOr(const nlohmann::json& parent, const char* key, int fallback) {
    auto retry = parent.find("retry");
    if (retry == parent.end() || !retry->is_object()) return fallback;
    auto it = retry->find(key);
    if (it == retry->end() || !it->is_number()) return fallback;
    return it->get<int>();
}

double RetryDoubleOr(const nlohmann::json& parent, const char* key, double fallback) {
    auto retry = parent.find("retry");
    if (retry == parent.end() || !retry->is_object()) return fallback;
    auto it = retry->find(key);
    if (it == retry->end() || !it->is_number()) return fallback;
    return it->get<double>();
}

double ClampedProbabilityOr(const nlohmann::json& parent, const char* key, double fallback) {
    const double value = ValueOr(parent, key, fallback);
    if (value < 0.0) return 0.0;
    if (value > 1.0) return 1.0;
    return value;
}

}  // namespace

std::string Settings::EffectiveEndpoint() const {
    if (!endpoint.empty()) return endpoint;
    return backend == Backend::OpenRouter ? kOpenRouterEndpoint : kTypesafeEndpoint;
}

std::string Settings::EffectiveModel() const {
    if (!model.empty()) return model;
    return backend == Backend::OpenRouter ? kOpenRouterModel : kTypesafeModel;
}

Settings ParseSettings(const nlohmann::json& config) {
    Settings settings;
    if (!config.is_object()) return settings;
    config::RejectUnknownKeys(
        config,
        {"backend", "api_key", "endpoint", "model", "request_timeout_ms", "budget_ms", "retry",
         "min_probability_allow", "min_probability_deny", "min_margin", "questions",
         "extra_questions"},
        "typesafe-advisor: config");
    if (const auto it = config.find("retry"); it != config.end()) {
        config::RejectUnknownKeys(*it,
                                  {"max_retries", "backoff_initial_ms", "backoff_max_ms", "jitter",
                                   "retry_after_cap_ms"},
                                  "typesafe-advisor: config.retry");
    }

    const std::string backend_name = ValueOr(config, "backend", std::string("typesafe"));
    settings.backend = backend_name == "openrouter" ? Backend::OpenRouter : Backend::Typesafe;

    // The backend default only applies when the entry names NO key at all;
    // an explicit api_key is honored verbatim on either backend.
    if (config.contains("api_key") && config.at("api_key").is_string()) {
        settings.api_key = config.at("api_key").get<std::string>();
    } else if (settings.backend == Backend::OpenRouter) {
        settings.api_key = "env:OPENROUTER_API_KEY";
    }

    settings.endpoint = ValueOr(config, "endpoint", settings.endpoint);
    settings.model = ValueOr(config, "model", settings.model);

    settings.request_timeout_ms =
        ValueOr(config, "request_timeout_ms", settings.request_timeout_ms);
    settings.budget_ms = ValueOr(config, "budget_ms", settings.budget_ms);
    if (settings.request_timeout_ms <= 0) settings.request_timeout_ms = 2000;
    if (settings.budget_ms <= 0) settings.budget_ms = 2000;
    // A per-attempt cap above the total budget can only mean "budget wins";
    // clamp it so the first attempt cannot silently outlive the loop.
    if (settings.request_timeout_ms > settings.budget_ms) {
        settings.request_timeout_ms = settings.budget_ms;
    }

    settings.max_retries = RetryIntOr(config, "max_retries", settings.max_retries);
    settings.backoff_initial_ms =
        RetryIntOr(config, "backoff_initial_ms", settings.backoff_initial_ms);
    settings.backoff_max_ms = RetryIntOr(config, "backoff_max_ms", settings.backoff_max_ms);
    settings.jitter = RetryDoubleOr(config, "jitter", settings.jitter);
    settings.retry_after_cap_ms =
        RetryIntOr(config, "retry_after_cap_ms", settings.retry_after_cap_ms);
    if (settings.max_retries < 0) settings.max_retries = 0;
    if (settings.backoff_initial_ms <= 0) settings.backoff_initial_ms = 1;
    if (settings.backoff_max_ms < settings.backoff_initial_ms) {
        settings.backoff_max_ms = settings.backoff_initial_ms;
    }
    if (settings.jitter < 0.0) settings.jitter = 0.0;
    if (settings.jitter > 1.0) settings.jitter = 1.0;
    if (settings.retry_after_cap_ms <= 0) settings.retry_after_cap_ms = 1000;

    settings.min_probability_allow =
        ClampedProbabilityOr(config, "min_probability_allow", settings.min_probability_allow);
    settings.min_probability_deny =
        ClampedProbabilityOr(config, "min_probability_deny", settings.min_probability_deny);
    settings.min_margin = ClampedProbabilityOr(config, "min_margin", settings.min_margin);

    if (auto it = config.find("questions"); it != config.end() && it->is_object()) {
        settings.questions = *it;
    }
    if (auto it = config.find("extra_questions"); it != config.end() && it->is_object()) {
        settings.extra_questions = *it;
    }

    return settings;
}

}  // namespace minimal::typesafe_advisor
