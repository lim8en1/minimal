#pragma once

// The one implementation of the LiteLLM fetch, shared by /cost-update-rates
// and the .rpc service's refresh_rates method. Opt-in network: only these two
// entry points fetch, never startup, never a turn.
//
// Runs on the read-only command pool or a gRPC handler thread -- blocking is
// acceptable there (beside a turn, never inside one), and the request is
// bounded by CostSettings::rates_fetch_timeout_ms.

#include <string>

#include "cost_model.hpp"
#include "cost_settings.hpp"
#include "cost_store.hpp"

namespace minimal::cost_tracker {

struct RatesFetchOutcome {
    bool ok = false;
    int updated = 0;         // model rates written, when ok
    std::string fetched_at;  // ISO-8601 UTC, when ok
    std::string error;       // REQUIRED when !ok
};

RatesFetchOutcome FetchAndUpdateRates(const CostSettings& settings, CostStore& store,
                                      RateTable& rates);

}  // namespace minimal::cost_tracker
