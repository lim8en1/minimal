#pragma once

// The one implementation of the snapshot fetch, shared by /cost-update-rates
// and the .rpc service's refresh_rates method. Two snapshots are fetched
// (LiteLLM's catalog, OpenRouter's own /api/v1/models list), each cached under
// its own storage key. Opt-in network: only these two entry points fetch,
// never startup, never a turn.
//
// Each source is best-effort: one failing never invalidates the other's
// stored snapshot -- a failed refresh degrades to the stale price, never to
// no price. Only when BOTH fail does the outcome carry an error.
//
// Runs on the read-only command pool or a gRPC handler thread -- blocking is
// acceptable there (beside a turn, never inside one), and each request is
// bounded by CostSettings::rates_fetch_timeout_ms.

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
