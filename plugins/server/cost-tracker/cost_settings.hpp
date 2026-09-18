#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace minimal::cost_tracker {

// This plugin's own config, the "config" object on its plugins.json entry.
// Core keeps it as raw JSON precisely so each plugin owns its own schema --
// this is that schema, and nothing outside cost-tracker needs to know it:
//
//   "config": {
//     "rates": {"gpt-4o": {"input": 2.5, "output": 10.0, "cache_read": 1.25}},
//     "rates_fetch_url": "https://...",
//     "openrouter_rates_fetch_url": "https://...",
//     "rates_fetch_timeout_ms": 30000,
//     "rates_refresh_days": 7
//   }
//
// `rates` is NOT parsed here: it is consumed verbatim as the RateTable's
// config tier (ParseRateTable owns the entry schema).
struct CostSettings {
    // The two pricing snapshots the plugin fetches (at server start when the
    // cached snapshot is stale, from --refresh-cost-rates, and through the
    // .rpc service's refresh_rates): LiteLLM's all-vendors catalog and
    // OpenRouter's own /api/v1/models list. Constant URLs by default so
    // pricing works with zero configuration; overridable for a mirror or a
    // pinned revision. The OpenRouter snapshot is keyed by OpenRouter slug --
    // which is exactly the wire model id an OpenRouter-shaped provider sends
    // -- so no provider naming or base-url sniffing is involved anywhere.
    std::string rates_fetch_url =
        "https://raw.githubusercontent.com/BerriAI/litellm/main/"
        "model_prices_and_context_window.json";
    std::string openrouter_rates_fetch_url = "https://openrouter.ai/api/v1/models";
    // Every fetch (the startup auto-refresh included) is bounded even when
    // the transfer stalls.
    int rates_fetch_timeout_ms = 30000;
    // How stale the cached snapshot may be before a server start fetches
    // again: 7 by default, 0 to fetch on every start. The CLI's
    // --refresh-cost-rates ignores this gate -- it always fetches.
    int rates_refresh_days = 7;
};

// Every field optional; missing values keep the struct's defaults, so an
// absent or partial config object is normal rather than an error.
CostSettings ParseCostSettings(const nlohmann::json& config);

}  // namespace minimal::cost_tracker
