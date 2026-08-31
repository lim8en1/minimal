#include "cost_model.hpp"

#include <algorithm>

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

constexpr double kTokensPerMtok = 1e6;

std::optional<double> PositiveNumber(const nlohmann::json& entry, const char* key) {
    const auto it = entry.find(key);
    if (it == entry.end() || !it->is_number()) return std::nullopt;
    const double value = it->get<double>();
    // Negative pricing is a hand-edit error, not a discount: treat as absent.
    if (value < 0.0) return std::nullopt;
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

std::map<std::string, Rate> IngestLiteLlmPrices(const nlohmann::json& document) {
    std::map<std::string, Rate> rates;
    if (!document.is_object()) return rates;
    for (auto it = document.begin(); it != document.end(); ++it) {
        // Skip LiteLLM's own metadata keys ("sample_spec", etc.) -- they carry
        // no per-token costs and fall out of the has-both-costs filter anyway.
        if (!it.value().is_object()) continue;
        const auto in = PositiveNumber(it.value(), kLiteLlmInput);
        const auto out = PositiveNumber(it.value(), kLiteLlmOutput);
        if (!in || !out) continue;
        Rate rate;
        rate.input_per_mtok = *in * kTokensPerMtok;
        rate.output_per_mtok = *out * kTokensPerMtok;
        if (const auto v = PositiveNumber(it.value(), kLiteLlmCacheRead)) {
            rate.cache_read_per_mtok = *v * kTokensPerMtok;
        }
        if (const auto v = PositiveNumber(it.value(), kLiteLlmCacheWrite)) {
            rate.cache_write_per_mtok = *v * kTokensPerMtok;
        }
        rates.emplace(it.key(), rate);
    }
    return rates;
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
    return nlohmann::json{{"fetched_at", rates.fetched_at}, {"models", std::move(models)}};
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
    rates.models = ParseRateTable(*models);
    return rates;
}

RateTable::RateTable(RateSources sources, plugins::PluginStorage* storage)
    : bundled_(ParseRateTable(sources.bundled_models)),
      config_(ParseRateTable(sources.config_rates)),
      fetched_(sources.fetched ? sources.fetched->models : std::map<std::string, Rate>{}),
      storage_(storage) {}

void RateTable::RefreshFetched() {
    std::map<std::string, Rate> fetched;
    if (storage_ != nullptr) {
        // Namespace is this plugin's own, so the blob is unreachable from
        // outside; the key is the one /cost-update-rates writes.
        if (const auto blob = storage_->Get(kStorageNamespace, kFetchedRatesKey)) {
            if (const auto rates = FetchedFromJson(*blob)) fetched = rates->models;
        }
    }
    const std::lock_guard<std::mutex> lock(mu_);
    fetched_ = std::move(fetched);
}

std::optional<Rate> RateTable::Resolve(const std::string& model) const {
    const std::lock_guard<std::mutex> lock(mu_);
    // config > fetched > bundled: the user's explicit price outranks a
    // snapshot, and a fresh snapshot outranks the stale shipped one.
    if (const auto it = config_.find(model); it != config_.end()) return it->second;
    if (const auto it = fetched_.find(model); it != fetched_.end()) return it->second;
    if (const auto it = bundled_.find(model); it != bundled_.end()) return it->second;
    return std::nullopt;
}

}  // namespace minimal::cost_tracker
