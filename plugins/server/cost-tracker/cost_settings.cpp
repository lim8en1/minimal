#include "cost_settings.hpp"

#include <limits>

#include "minimal/common/config_reader.hpp"
#include "minimal/common/plugin_config_label.hpp"

namespace minimal::cost_tracker {

CostSettings ParseCostSettings(const nlohmann::json& config) {
    CostSettings settings;
    config::Reader reader(config, common::PluginConfigLabel(common::kPluginConfigDirName, "cost-tracker"));
    reader.Describe("rates_fetch_url", {"Pricing snapshot source (LiteLLM's model price table)."});
    reader.Describe("openrouter_rates_fetch_url", {"OpenRouter's own pricing source."});
    reader.Describe("rates_fetch_timeout_ms", {"Bound on one pricing fetch."});
    reader.Describe("rates_refresh_days",
                    {"Snapshot age that triggers a refetch at startup; 0 = every start."});
    // Live: rates (the price lookup is per report). Restart: the fetch
    // settings (held by the refresher and the RPC service) and
    // rates_refresh_days, which only a startup consults.
    reader.Describe("rates", {"Per-model rate overrides, keyed by model id: {\"input\", "
                              "\"output\", \"cache_read\"} per million tokens. Costs are priced "
                              "when reported, so a change reprices past usage too.",
                              config::ApplyPolicy::kLive});
    reader.Default("rates", nlohmann::json::object());
    settings.rates_fetch_url =
        reader.String("rates_fetch_url", settings.rates_fetch_url, /*allow_empty=*/false);
    settings.openrouter_rates_fetch_url = reader.String(
        "openrouter_rates_fetch_url", settings.openrouter_rates_fetch_url, /*allow_empty=*/false);
    settings.rates_fetch_timeout_ms = static_cast<int>(reader.Int(
        "rates_fetch_timeout_ms", settings.rates_fetch_timeout_ms, 1, 600000));
    // 0 is meaningful ("always refresh").
    settings.rates_refresh_days = static_cast<int>(
        reader.Int("rates_refresh_days", settings.rates_refresh_days, 0, 3650));
    if (auto rates = reader.MaybeObject("rates")) settings.rates = rates->node();
    reader.Finish();
    return settings;
}

}  // namespace minimal::cost_tracker
