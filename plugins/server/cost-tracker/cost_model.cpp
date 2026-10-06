#include "cost_model.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace minimal::cost_tracker {
namespace {

constexpr const char* kInputKey = "input";
constexpr const char* kOutputKey = "output";
constexpr const char* kCacheReadKey = "cache_read";
constexpr const char* kCacheWriteKey = "cache_write";

constexpr const char* kLiteLlmInput = "input_cost_per_token";
constexpr const char* kLiteLlmOutput = "output_cost_per_token";
constexpr const char* kLiteLlmCacheRead = "cache_read_input_token_cost";
constexpr const char* kLiteLlmCacheWrite = "cache_creation_input_token_cost";

constexpr double kTokensPerMtok = 1e6;  // NOLINT(readability-magic-numbers): prices are per million

std::optional<double> PositiveNumber(const nlohmann::json& entry, const char* key) {
    const auto it = entry.find(key);
    if (it == entry.end() || !it->is_number()) return std::nullopt;
    const double value = it->get<double>();
    // Negative pricing is a hand-edit error, not a discount; a non-finite
    // value (inf/nan from an out-of-range float literal) is corruption.
    // Both are treated as absent -- CostFor would otherwise produce an
    // infected total that renders as $inf or $nan.
    if (!(value >= 0.0) || !std::isfinite(value)) return std::nullopt;
    return value;
}

constexpr const char* kOpenRouterPrompt = "prompt";
constexpr const char* kOpenRouterCompletion = "completion";
constexpr const char* kOpenRouterCacheRead = "input_cache_read";
constexpr const char* kOpenRouterCacheWrite = "input_cache_write";

// OpenRouter prices every field as a STRING of a decimal ("0.00000015");
// a JSON number is accepted too, for tolerance against a format drift.
// Same reject rules as PositiveNumber: negative, non-finite, or (for the
// string form) anything not fully consumed as one decimal number.
std::optional<double> OpenRouterPrice(const nlohmann::json& pricing, const char* key) {
    const auto it = pricing.find(key);
    if (it == pricing.end()) return std::nullopt;
    double value = 0.0;
    if (it->is_number()) {
        value = it->get<double>();
    } else if (it->is_string()) {
        const std::string text = it->get<std::string>();
        try {
            std::size_t consumed = 0;
            value = std::stod(text, &consumed);
            if (consumed != text.size()) return std::nullopt;
        } catch (const std::exception&) {
            return std::nullopt;
        }
    } else {
        return std::nullopt;
    }
    if (!(value >= 0.0) || !std::isfinite(value)) return std::nullopt;
    return value;
}

}  // namespace

double CostFor(const Rate& rate, const UsageRecord& usage) {
    if (!usage.usage_reported) return 0.0;
    const double in =
        static_cast<double>(usage.input_tokens) / kTokensPerMtok * rate.input_per_mtok;
    const double out =
        static_cast<double>(usage.output_tokens) / kTokensPerMtok * rate.output_per_mtok;
    const double cr = static_cast<double>(usage.cache_read_tokens) / kTokensPerMtok *
                      rate.cache_read_per_mtok;
    const double cw = static_cast<double>(usage.cache_write_tokens) / kTokensPerMtok *
                      rate.cache_write_per_mtok;
    return in + out + cr + cw;
}

SessionTotals Aggregate(const std::string& session_id, const std::string& parent_session_id,
                        const std::vector<TurnRecord>& turns, const RateResolver& resolve) {
    std::map<std::string, ModelTotals> by_model;
    for (const auto& turn : turns) {
        auto& totals = by_model[turn.model];
        totals.model = turn.model;
        ++totals.turns;
        if (!turn.usage.usage_reported) {
            // The turn happened; its size is simply unknown (the zero-usage
            // proxy gap). Counted, never priced.
            ++totals.unpriced_turns;
            totals.cost_known = false;
            continue;
        }
        totals.input_tokens += turn.usage.input_tokens;
        totals.output_tokens += turn.usage.output_tokens;
        totals.cache_read_tokens += turn.usage.cache_read_tokens;
        totals.cache_write_tokens += turn.usage.cache_write_tokens;
        const auto rate = resolve(turn.model);
        if (!rate) {
            ++totals.unpriced_turns;
            totals.cost_known = false;
            continue;
        }
        totals.cost += CostFor(*rate, turn.usage);
    }

    SessionTotals result;
    result.session_id = session_id;
    result.parent_session_id = parent_session_id;
    result.models.reserve(by_model.size());
    for (auto& [model, totals] : by_model) {
        result.total_cost += totals.cost;
        if (!totals.cost_known) result.total_cost_known = false;
        result.total_turns += totals.turns;
        result.unpriced_turns += totals.unpriced_turns;
        result.models.push_back(std::move(totals));
    }
    // std::map iteration is already model-sorted; keep that visible.
    std::sort(result.models.begin(), result.models.end(),
              [](const ModelTotals& a, const ModelTotals& b) { return a.model < b.model; });
    return result;
}

std::map<std::string, Rate> ParseRateTable(const nlohmann::json& models_object) {
    std::map<std::string, Rate> rates;
    if (!models_object.is_object()) return rates;
    for (auto it = models_object.begin(); it != models_object.end(); ++it) {
        if (!it.value().is_object()) continue;
        Rate rate;
        // Every field optional -- a cache-free model carries only input/output.
        if (const auto v = PositiveNumber(it.value(), kInputKey)) rate.input_per_mtok = *v;
        if (const auto v = PositiveNumber(it.value(), kOutputKey)) rate.output_per_mtok = *v;
        if (const auto v = PositiveNumber(it.value(), kCacheReadKey)) rate.cache_read_per_mtok = *v;
        if (const auto v = PositiveNumber(it.value(), kCacheWriteKey)) {
            rate.cache_write_per_mtok = *v;
        }
        rates.emplace(it.key(), rate);
    }
    return rates;
}

namespace {

// One entry in LiteLLM's per-token field schema, shared by the catalog and the
// proxy's /model/info. nullopt when the entry is not a text-chat price: no
// usable input cost, or (when `require_output`) no usable output cost.
std::optional<Rate> LiteLlmEntryRate(const nlohmann::json& entry, bool require_output) {
    const auto in = PositiveNumber(entry, kLiteLlmInput);
    const auto out = PositiveNumber(entry, kLiteLlmOutput);
    if (!in || (require_output && !out)) return std::nullopt;
    Rate rate;
    rate.input_per_mtok = *in * kTokensPerMtok;
    if (out) rate.output_per_mtok = *out * kTokensPerMtok;
    if (const auto v = PositiveNumber(entry, kLiteLlmCacheRead)) {
        rate.cache_read_per_mtok = *v * kTokensPerMtok;
    }
    if (const auto v = PositiveNumber(entry, kLiteLlmCacheWrite)) {
        rate.cache_write_per_mtok = *v * kTokensPerMtok;
    }
    return rate;
}

}  // namespace

std::map<std::string, Rate> IngestLiteLlmPrices(const nlohmann::json& document) {
    std::map<std::string, Rate> rates;
    if (!document.is_object()) return rates;
    for (auto it = document.begin(); it != document.end(); ++it) {
        // Skip LiteLLM's own metadata keys ("sample_spec", etc.) -- they carry
        // no per-token costs and fall out of the has-both-costs filter anyway.
        if (!it.value().is_object()) continue;
        if (const auto rate = LiteLlmEntryRate(it.value(), /*require_output=*/true)) {
            rates.emplace(it.key(), *rate);
        }
    }
    return rates;
}

std::map<std::string, Rate> IngestLiteLlmProxyPrices(const nlohmann::json& document) {
    std::map<std::string, Rate> rates;
    if (!document.is_object()) return rates;
    const auto data = document.find("data");
    if (data == document.end() || !data->is_array()) return rates;
    for (const auto& entry : *data) {
        if (!entry.is_object()) continue;
        const auto name = entry.find("model_name");
        if (name == entry.end() || !name->is_string() || name->get<std::string>().empty()) {
            continue;
        }
        const auto info = entry.find("model_info");
        if (info == entry.end() || !info->is_object()) continue;
        if (const auto rate = LiteLlmEntryRate(*info, /*require_output=*/false)) {
            rates.emplace(name->get<std::string>(), *rate);
        }
    }
    return rates;
}

std::map<std::string, Rate> IngestOpenRouterPrices(const nlohmann::json& document) {
    std::map<std::string, Rate> rates;
    const auto data = document.find("data");
    if (data == document.end() || !data->is_array()) return rates;
    for (const auto& entry : *data) {
        if (!entry.is_object()) continue;
        const auto id = entry.find("id");
        if (id == entry.end() || !id->is_string() || id->get<std::string>().empty()) continue;
        const auto pricing = entry.find("pricing");
        if (pricing == entry.end() || !pricing->is_object()) continue;
        const auto in = OpenRouterPrice(*pricing, kOpenRouterPrompt);
        const auto out = OpenRouterPrice(*pricing, kOpenRouterCompletion);
        // Same both-fields rule as LiteLLM: a row without a usable pair of
        // per-token prices is not a text-chat price at all.
        if (!in || !out) continue;
        Rate rate;
        rate.input_per_mtok = *in * kTokensPerMtok;
        rate.output_per_mtok = *out * kTokensPerMtok;
        if (const auto v = OpenRouterPrice(*pricing, kOpenRouterCacheRead)) {
            rate.cache_read_per_mtok = *v * kTokensPerMtok;
        }
        if (const auto v = OpenRouterPrice(*pricing, kOpenRouterCacheWrite)) {
            rate.cache_write_per_mtok = *v * kTokensPerMtok;
        }
        rates.emplace(id->get<std::string>(), rate);
    }
    return rates;
}

const char* RateFormatName(RateFormat format) {
    switch (format) {
        case RateFormat::kLiteLlm: return "litellm";
        case RateFormat::kOpenRouter: return "openrouter";
        case RateFormat::kLiteLlmProxy: return "litellm-proxy";
    }
    return "";
}

std::map<std::string, Rate> IngestPrices(RateFormat format, const nlohmann::json& document) {
    switch (format) {
        case RateFormat::kLiteLlm: return IngestLiteLlmPrices(document);
        case RateFormat::kOpenRouter: return IngestOpenRouterPrices(document);
        case RateFormat::kLiteLlmProxy: return IngestLiteLlmProxyPrices(document);
    }
    return {};
}

nlohmann::json FetchedToJson(const FetchedRates& rates) {
    nlohmann::json models = nlohmann::json::object();
    for (const auto& [model, rate] : rates.models) {
        models[model] = {
            {kInputKey, rate.input_per_mtok},
            {kOutputKey, rate.output_per_mtok},
            {kCacheReadKey, rate.cache_read_per_mtok},
            {kCacheWriteKey, rate.cache_write_per_mtok},
        };
    }
    return nlohmann::json{{"fetched_at", rates.fetched_at},
                          {"fetched_at_unix", rates.fetched_at_unix},
                          {"models", std::move(models)}};
}

std::optional<FetchedRates> FetchedFromJson(std::string_view json) {
    nlohmann::json document;
    try {
        document = nlohmann::json::parse(json);
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (!document.is_object()) return std::nullopt;
    const auto models = document.find("models");
    if (models == document.end() || !models->is_object()) return std::nullopt;
    FetchedRates rates;
    if (const auto at = document.find("fetched_at"); at != document.end() && at->is_string()) {
        rates.fetched_at = at->get<std::string>();
    }
    // Tolerant of absence: blobs written before the field existed decode with
    // the struct's 0, which every staleness consumer treats as stale.
    if (const auto at = document.find("fetched_at_unix");
        at != document.end() && at->is_number_integer()) {
        rates.fetched_at_unix = at->get<std::int64_t>();
    }
    rates.models = ParseRateTable(*models);
    return rates;
}

RateTable::RateTable(RateSources sources, plugins::PluginStorage* storage)
    : bundled_(ParseRateTable(sources.bundled_models)),
      config_(ParseRateTable(sources.config_rates)),
      fetched_sources_(std::move(sources.fetched_sources)),
      fetched_(fetched_sources_.size()),
      storage_(storage) {}

void RateTable::SetConfigRates(const nlohmann::json& config_rates) {
    auto parsed = ParseRateTable(config_rates);
    std::lock_guard<std::mutex> lock(mu_);
    config_ = std::move(parsed);
}

void RateTable::RefreshFetched() {
    std::vector<std::map<std::string, Rate>> fetched(fetched_sources_.size());
    if (storage_ != nullptr) {
        // Namespace is this plugin's own, so the blobs are unreachable from
        // outside; the keys are the ones the rates fetch writes. Each tier
        // decodes independently: one corrupt snapshot must not lose the others.
        for (std::size_t i = 0; i < fetched_sources_.size(); ++i) {
            const auto blob =
                storage_->Get(kStorageNamespace, FetchedRatesKey(fetched_sources_[i]));
            if (!blob) continue;
            if (auto rates = FetchedFromJson(*blob)) fetched[i] = std::move(rates->models);
        }
    }
    {
        const std::lock_guard<std::mutex> lock(mu_);
        fetched_ = std::move(fetched);
    }
}

std::optional<Rate> RateTable::Resolve(const std::string& model) const {
    const std::lock_guard<std::mutex> lock(mu_);
    // config > fetched sources in list order > bundled: the user's explicit
    // price outranks every snapshot, the configured order ranks the
    // snapshots, and any snapshot outranks the stale shipped one.
    if (const auto it = config_.find(model); it != config_.end()) return it->second;
    for (const auto& tier : fetched_) {
        if (const auto it = tier.find(model); it != tier.end()) return it->second;
    }
    if (const auto it = bundled_.find(model); it != bundled_.end()) return it->second;
    return std::nullopt;
}

}  // namespace minimal::cost_tracker
