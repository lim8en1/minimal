#pragma once

// The one implementation of the snapshot fetch, shared by the startup
// auto-refresh, the server's --plugin-call one-shot, and the .rpc
// service's refresh_rates method. Two snapshots are fetched (LiteLLM's
// catalog, OpenRouter's own /api/v1/models list), each cached under its own
// storage key. The fetches are bounded (rates_fetch_timeout_ms per source)
// and run beside a turn at worst -- never inside one.
//
// Each source is best-effort: one failing never invalidates the other's
// stored snapshot -- a failed refresh degrades to the stale price, never to
// no price. Only when BOTH fail does the outcome carry an error.
//
// Runs on a plugin-owned background thread (the startup auto-refresh) or a
// gRPC handler thread (--plugin-call cost-tracker refresh_rates / refresh_rates) -- blocking is
// acceptable there.

#include <string>

#include "cost_model.hpp"
#include "cost_settings.hpp"
#include "cost_store.hpp"

namespace minimal::cost_tracker {

struct RatesFetchOutcome {
    bool ok = false;
    int updated = 0;              // total model rates written, when ok
    int litellm_updated = 0;      // of `updated`, the LiteLLM snapshot's share
    int openrouter_updated = 0;   // ... and the OpenRouter snapshot's
    std::string fetched_at;       // ISO-8601 UTC, when ok
    std::string warning;          // set when one source failed but the other refreshed
    std::string error;            // REQUIRED when !ok
};

RatesFetchOutcome FetchAndUpdateRates(const CostSettings& settings, CostStore& store,
                                      RateTable& rates);

}  // namespace minimal::cost_tracker
