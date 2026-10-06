#pragma once

// The one implementation of the snapshot fetch, shared by the startup
// auto-refresh, the server's --plugin-call one-shot, and the .rpc
// service's refresh_rates method. Every source in CostSettings::rate_sources
// is fetched, each cached under its own storage key. The fetches are bounded
// (rates_fetch_timeout_ms per source) and run beside a turn at worst -- never
// inside one.
//
// Each source is best-effort: one failing never invalidates another's stored
// snapshot -- a failed refresh degrades to the stale price, never to no
// price. Only when EVERY source fails does the outcome carry an error.
//
// Runs on a plugin-owned background thread (the startup auto-refresh) or a
// gRPC handler thread (--plugin-call cost-tracker refresh_rates / refresh_rates) -- blocking is
// acceptable there.

#include <string>
#include <vector>

#include "cost_model.hpp"
#include "cost_settings.hpp"
#include "cost_store.hpp"

namespace minimal::cost_tracker {

// One source's result, in rate_sources order.
struct SourceFetchOutcome {
    std::string name;
    int updated = 0;    // model rates written; 0 when the source failed
    std::string error;  // the fetch or cache failure, when it failed
};

struct RatesFetchOutcome {
    bool ok = false;
    int updated = 0;                         // total model rates written, when ok
    std::vector<SourceFetchOutcome> sources;
    std::string fetched_at;                  // ISO-8601 UTC, when ok
    std::string warning;                     // set when some sources failed but one refreshed
    std::string error;                       // REQUIRED when !ok
};

RatesFetchOutcome FetchAndUpdateRates(const CostSettings& settings, CostStore& store,
                                      RateTable& rates);

}  // namespace minimal::cost_tracker
