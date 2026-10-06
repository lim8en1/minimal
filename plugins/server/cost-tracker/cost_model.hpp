#pragma once

// Cost arithmetic and aggregation for the cost-tracker plugin. Pure logic --
// no Tool, no RPC, no storage -- so the tests exercise it directly.
//
// RATE SOURCES, in resolution order (first hit wins):
//   1. the plugin's own config slice in plugins.json ("rates") -- the user's
//      explicit prices, so they outrank everything;
//   2. the fetched OpenRouter snapshot (its own /api/v1/models list), cached
//      in this plugin's storage by the startup auto-refresh (a server start
//      fetches when the cached snapshot is older than rates_refresh_days;
//      --plugin-call cost-tracker refresh_rates and the .rpc refresh_rates method fetch on
//      demand);
//   3. the fetched LiteLLM snapshot, the same paths, the same rule;
//   4. the seeded bundled snapshot (cost-rates.json in the config dir).
//
// The OpenRouter tier sits above LiteLLM because the wire ids an
// OpenRouter-shaped provider sends ARE OpenRouter slugs ("z-ai/glm-5.3-flash"
// -- the key inside providers.json's models map, whatever the provider is
// named), so OpenRouter's own price for that exact string is the authoritative
// one; LiteLLM's "openrouter/*" entries duplicate the same data and lag it.
//
// KEYS ARE MODEL IDS. The observer payload carries the request's model name
// and nothing else (TurnSummary::model), so a rate is keyed by the model id
// the user's providers.json names -- the ccusage convention. Two providers
// serving the same model id share a rate; that ambiguity is accepted, and a
// user who needs to distinguish them overrides the rate in the config slice
// (which is keyed the same way -- the limitation is documented, not hidden).
//
// ZERO-USAGE HONESTY. Some OpenAI-compatible proxies report all-zero usage
// (a known harness gap, ARCHITECTURE.md "Deferred"). A turn whose usage sums
// to zero is recorded with usage_reported = false and contributes "unknown",
// never $0.00: a missing price is not a price of zero.

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "minimal/plugins/plugin_storage.hpp"

namespace minimal::cost_tracker {

inline constexpr char kPluginName[] = "cost-tracker";
// The PluginStorage namespace every row of this plugin lives under.
inline constexpr char kStorageNamespace[] = "cost-tracker";

// Storage layout keys. Session history is one blob per session under
// "sessions/<session_id>"; the two fetched-rates snapshots the startup
// auto-refresh writes and RateTable's middle tiers read live at
// "rates/fetched" (LiteLLM) and "rates/fetched_openrouter".
inline constexpr char kSessionKeyPrefix[] = "sessions/";
inline constexpr char kFetchedRatesKey[] = "rates/fetched";
inline constexpr char kOpenRouterFetchedRatesKey[] = "rates/fetched_openrouter";

// USD per million tokens. Absent fields are 0, which is correct for models
// without cache pricing -- it is also what a genuinely free tier looks like,
// but a free tier still reports usage, so "known" stays true. The distinction
// that matters is usage_reported below, not a zero rate.
struct Rate {
    double input_per_mtok = 0.0;
    double output_per_mtok = 0.0;
    double cache_read_per_mtok = 0.0;
    double cache_write_per_mtok = 0.0;
};

// One turn's usage, normalized from the observer payload.
struct UsageRecord {
    std::int64_t input_tokens = 0;
    std::int64_t output_tokens = 0;
    std::int64_t cache_read_tokens = 0;
    std::int64_t cache_write_tokens = 0;
    // False when the backend reported all zeros: the turn happened but its
    // size is unknown, so it is EXCLUDED from cost and surfaced separately.
    bool usage_reported = true;
};

struct TurnRecord {
    std::string model;  // empty only for a turn that failed before a request was built
    std::int64_t at_unix_ms = 0;
    // Transcript bounds, from TurnSummary. -1 is the "unknown" sentinel: blobs
    // written before these fields existed decode without them, and a record
    // carrying the sentinel cannot be classified on rewind (kept, never cut).
    std::int64_t first_seq = -1;
    std::int64_t last_seq = -1;
    UsageRecord usage;
};

struct ModelTotals {
    std::string model;
    std::int64_t input_tokens = 0;
    std::int64_t output_tokens = 0;
    std::int64_t cache_read_tokens = 0;
    std::int64_t cache_write_tokens = 0;
    int turns = 0;
    // Turns of this model that could not be priced: unreported usage, or no
    // rate for the model id. Cost covers only the priced turns; this counter
    // is what keeps the report honest about the rest.
    int unpriced_turns = 0;
    double cost = 0.0;
    bool cost_known = true;  // false when unpriced_turns > 0
};

struct SessionTotals {
    std::string session_id;
    std::string parent_session_id;
    std::vector<ModelTotals> models;  // sorted by model id
    double total_cost = 0.0;
    bool total_cost_known = true;
    int total_turns = 0;
    int unpriced_turns = 0;
};

double CostFor(const Rate& rate, const UsageRecord& usage);

// Aggregation is driven through a resolver rather than RateTable itself so
// the pure-logic tier can test it without a rate table or storage.
using RateResolver = std::function<std::optional<Rate>(const std::string& model)>;

SessionTotals Aggregate(const std::string& session_id, const std::string& parent_session_id,
                        const std::vector<TurnRecord>& turns, const RateResolver& resolve);

// --- Rate sources -----------------------------------------------------------

// Our rate schema: {"input": 2.5, "output": 10.0, "cache_read": 1.25,
// "cache_write": 0} -- USD per million tokens. Non-number entries and
// non-object values are skipped rather than fatal: a hand-edited file with one
// bad row must not lose every other model's price.
std::map<std::string, Rate> ParseRateTable(const nlohmann::json& models_object);

// LiteLLM's model_prices_and_context_window.json is keyed by model id and
// prices per TOKEN (input_cost_per_token et al). Kept entries are those with
// numeric input and output costs; everything else (per-image, per-second,
// embedding-only) is skipped. Normalized to per-mtok on the way in.
std::map<std::string, Rate> IngestLiteLlmPrices(const nlohmann::json& document);

// OpenRouter's /api/v1/models reply: {"data": [{"id": "vendor/model",
// "pricing": {"prompt": "0.00000015", "completion": "0.0000005", ...}}, ...]}.
// Prices are per-token USD as JSON STRINGS (a numeric value is accepted for
// tolerance). Kept entries are those with a string id and parseable prompt +
// completion; free tiers ("0") ingest as a genuine $0 rate. Normalized to
// per-mtok on the way in.
std::map<std::string, Rate> IngestOpenRouterPrices(const nlohmann::json& document);

struct FetchedRates {
    // ISO-8601 UTC, diagnostic only.
    std::string fetched_at;
    // Unix seconds, the machine-usable twin of fetched_at -- the staleness
    // gate the startup auto-refresh checks. 0 (a blob written before this
    // field existed) counts as stale, never fresh.
    std::int64_t fetched_at_unix = 0;
    std::map<std::string, Rate> models;
};

nlohmann::json FetchedToJson(const FetchedRates& rates);
// nullopt when the blob is absent, malformed, or lacks the models object --
// a corrupt cache is re-fetchable state, not a crash.
std::optional<FetchedRates> FetchedFromJson(std::string_view json);

// Bundled/config rate tables as raw "models" objects -- ParseRateTable
// consumes them. Kept as JSON rather than pre-parsed maps so the table is
// re-derivable and the resolver stays the single decision point.
struct RateSources {
    nlohmann::json bundled_models = nlohmann::json::object();
    nlohmann::json config_rates = nlohmann::json::object();
    std::optional<FetchedRates> fetched;
    std::optional<FetchedRates> openrouter_fetched;
};

// Resolution: config > openrouter-fetched > litellm-fetched > bundled.
// Thread-safe: /cost and the .rpc service read from command-pool and gRPC
// threads while the startup auto-refresh (and any on-demand fetch) swaps the
// fetched tables.
class RateTable {
public:
    explicit RateTable(RateSources sources, plugins::PluginStorage* storage);

    std::optional<Rate> Resolve(const std::string& model) const;

    // Re-reads both fetched snapshots from storage and swaps them in. Called
    // at registration and after every fetch (the startup auto-refresh, the
    // --plugin-call cost-tracker refresh_rates one-shot, the .rpc refresh_rates method). A
    // missing or corrupt blob clears only THAT tier -- one corrupt snapshot
    // must not lose the other.
    void RefreshFetched();

    // Replaces the config tier (the plugin's `rates`): a live reconfigure.
    // Costs are priced when reported, so it reprices past usage too.
    void SetConfigRates(const nlohmann::json& config_rates);

private:
    mutable std::mutex mu_;
    std::map<std::string, Rate> bundled_;
    std::map<std::string, Rate> config_;
    std::map<std::string, Rate> fetched_;
    std::map<std::string, Rate> openrouter_fetched_;
    plugins::PluginStorage* storage_;  // borrowed; may be null
};

}  // namespace minimal::cost_tracker
