#include "settings.hpp"


#include "minimal/common/config_reader.hpp"

namespace minimal::typesafe_advisor {
namespace {

constexpr const char* kTypesafeEndpoint = "https://api.typesafe.ai/v1/systemone";
constexpr const char* kTypesafeModel = "jev-latest";
constexpr const char* kOpenRouterEndpoint = "https://openrouter.ai/api/alpha/decisions";
constexpr const char* kOpenRouterModel = "~typesafe/jev-latest";

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
    constexpr std::int64_t kMaxMs = 600000;
    config::Reader reader(config, "plugins.json", "typesafe-advisor.config");
    settings.backend = reader.Enum("backend", "typesafe", {"typesafe", "openrouter"}) == "openrouter"
                           ? Backend::OpenRouter
                           : Backend::Typesafe;
    // Required, never defaulted: a key is only ever sent where it is named,
    // and a backend typo must not route one service's key to the other.
    settings.api_key = reader.RequireString("api_key");
    settings.endpoint = reader.String("endpoint", settings.endpoint);
    settings.model = reader.String("model", settings.model);

    settings.request_timeout_ms = static_cast<int>(
        reader.Int("request_timeout_ms", settings.request_timeout_ms, 1, kMaxMs));
    settings.budget_ms = static_cast<int>(reader.Int("budget_ms", settings.budget_ms, 1, kMaxMs));
    // A per-attempt cap above the total budget can only mean "budget wins";
    // the first attempt must not be able to outlive the loop.
    if (settings.request_timeout_ms > settings.budget_ms) {
        reader.Fail("request_timeout_ms", "must not exceed budget_ms");
    }

    config::Reader retry = reader.Object("retry");
    settings.max_retries =
        static_cast<int>(retry.Int("max_retries", settings.max_retries, 0, 100));
    settings.backoff_initial_ms = static_cast<int>(
        retry.Int("backoff_initial_ms", settings.backoff_initial_ms, 1, kMaxMs));
    settings.backoff_max_ms =
        static_cast<int>(retry.Int("backoff_max_ms", settings.backoff_max_ms, 1, kMaxMs));
    settings.jitter = retry.Double("jitter", settings.jitter, 0.0, 1.0);
    settings.retry_after_cap_ms = static_cast<int>(
        retry.Int("retry_after_cap_ms", settings.retry_after_cap_ms, 1, kMaxMs));
    retry.Finish();
    if (settings.backoff_max_ms < settings.backoff_initial_ms) {
        reader.Fail("retry.backoff_max_ms", "must not be below retry.backoff_initial_ms");
    }

    settings.min_probability_allow =
        reader.Double("min_probability_allow", settings.min_probability_allow, 0.0, 1.0);
    settings.min_probability_deny =
        reader.Double("min_probability_deny", settings.min_probability_deny, 0.0, 1.0);
    settings.min_margin = reader.Double("min_margin", settings.min_margin, 0.0, 1.0);
    if (auto questions = reader.MaybeObject("questions")) settings.questions = questions->node();
    if (auto extra = reader.MaybeObject("extra_questions")) settings.extra_questions = extra->node();
    reader.Finish();
    return settings;
}

}  // namespace minimal::typesafe_advisor
