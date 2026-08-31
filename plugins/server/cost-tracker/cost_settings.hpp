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
//     "rates_fetch_timeout_ms": 30000
//   }
//
// `rates` is NOT parsed here: it is consumed verbatim as the RateTable's
// config tier (ParseRateTable owns the entry schema).
struct CostSettings {
    // The LiteLLM pricing snapshot /cost-update-rates fetches. A constant URL
    // by default so the command is useful with zero configuration; overridable
    // for a mirror or a pinned revision.
    std::string rates_fetch_url =
        "https://raw.githubusercontent.com/BerriAI/litellm/main/"
        "model_prices_and_context_window.json";
    // The fetch runs on the read-only command pool beside a live turn, so it
    // must be bounded even when the transfer stalls.
    int rates_fetch_timeout_ms = 30000;
};

// Every field optional; missing values keep the struct's defaults, so an
// absent or partial config object is normal rather than an error.
CostSettings ParseCostSettings(const nlohmann::json& config);

}  // namespace minimal::cost_tracker
