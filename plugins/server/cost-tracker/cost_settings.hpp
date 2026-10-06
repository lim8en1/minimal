#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include "cost_model.hpp"
#include "minimal/common/secret_handle.hpp"
#include "minimal/config/secrets.hpp"
#include "policy.hpp"

namespace minimal::cost_tracker {

// One pricing snapshot the plugin fetches. `name` is the snapshot's storage
// key suffix and its label in logs and the refresh_rates reply, so it is
// unique within the list; `format` picks the ingest.
struct RateSourceSettings {
    std::string name;
    RateFormat format = RateFormat::kLiteLlm;
    std::string url;
    // A key reference ("secret:NAME", "env:NAME", "secret:<plugin>:NAME", or a
    // literal) sent as Authorization: Bearer; empty = no header. Bound to
    // `url`'s host at parse time, read per fetch -- a secret stored later
    // reaches the next refresh.
    std::string api_key_ref;
    common::SecretHandle api_key;
};

// The default list: OpenRouter's own /api/v1/models (keyed by OpenRouter slug
// -- exactly the wire model id an OpenRouter-shaped provider sends, so no
// provider naming or base-url sniffing is involved anywhere), then LiteLLM's
// all-vendors catalog. Constant URLs so pricing works with zero configuration.
std::vector<RateSourceSettings> DefaultRateSources();

// This plugin's own config, the "config" object on its plugins.json entry.
// Core keeps it as raw JSON precisely so each plugin owns its own schema --
// this is that schema, and nothing outside cost-tracker needs to know it:
//
//   "config": {
//     "rates": {"gpt-4o": {"input": 2.5, "output": 10.0, "cache_read": 1.25}},
//     "rate_sources": [
//       {"format": "litellm-proxy", "url": "https://gw/model/info",
//        "api_key": "secret:gateway"},
//       {"format": "openrouter", "url": "https://openrouter.ai/api/v1/models"},
//       {"name": "litellm-mirror", "format": "litellm", "url": "https://..."}
//     ],
//     "rates_fetch_timeout_ms": 30000,
//     "rates_refresh_days": 7
//   }
//
// A source's `name` defaults to its `format`, so a list naming each format
// once needs no names. `rates` is NOT parsed here: it is consumed verbatim as
// the RateTable's config tier (ParseRateTable owns the entry schema).
struct CostSettings {
    // The pricing snapshots the plugin fetches (at server start when a cached
    // snapshot is stale, from --plugin-call cost-tracker refresh_rates, and
    // through the .rpc service's refresh_rates), highest priority first: a
    // model priced by several resolves to the earliest. Replacing the list
    // replaces it whole; an empty list fetches nothing.
    std::vector<RateSourceSettings> rate_sources = DefaultRateSources();
    // Every fetch (the startup auto-refresh included) is bounded even when
    // the transfer stalls.
    int rates_fetch_timeout_ms = static_cast<int>(kDefaultRatesFetchTimeout.count());
    // How stale the cached snapshot may be before a server start fetches
    // again: 7 by default, 0 to fetch on every start. The CLI's
    // --plugin-call cost-tracker refresh_rates ignores this gate -- it always fetches.
    int rates_refresh_days = kDefaultRatesRefreshDays;
    // The operator's own price table ({"<model>": {...}}), highest precedence
    // over the bundled and fetched ones; parsed by the rate table itself.
    nlohmann::json rates = nlohmann::json::object();

    // The fetched tiers' names, in priority order -- what RateTable needs.
    std::vector<std::string> RateSourceNames() const;
};

// Every field optional; a missing field keeps the struct's default. A present
// field of the wrong type or out of range, an unknown field, a duplicate
// source name, or a malformed api_key reference throws config::ConfigError
// naming it -- the plugin then fails to load, loudly. `secrets` null = the
// default resolver (the operator's store, no plugin namespaces).
CostSettings ParseCostSettings(const nlohmann::json& config,
                               const config::SecretResolver* secrets = nullptr);

}  // namespace minimal::cost_tracker
