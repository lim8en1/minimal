#pragma once

// The cost tracker's §2.2 cross-plugin service ("ask, don't grab"): a typed,
// read-only view of the same store the observer writes and /cost reads.
//
// Consumers resolve it through ServiceRegistry::Find(kCostTrackingServiceName)
// and static_cast the result to this interface. The name is the only runtime
// coupling; this header is the contract, under the same source-compatibility
// model as every other shipped interface. The planned first consumer is a
// client-plugin view over the PluginCall channel -- but that goes through the
// plugin's .rpc dispatch target, not through this interface; this is for
// OTHER SERVER-SIDE PLUGINS that want totals in-process (a status aggregator,
// a spend-cap advisor).

#include <optional>
#include <string>
#include <vector>

#include "cost_model.hpp"

namespace minimal::cost_tracker {

inline constexpr char kCostTrackingServiceName[] = "cost-tracker.tracking";

class CostTrackingService {
public:
    virtual ~CostTrackingService() = default;

    // nullopt when the session has no recorded turns (or storage is absent --
    // "no data", not an error, per the service-channel contract).
    virtual std::optional<SessionTotals> Totals(const std::string& session_id) const = 0;

    // Every session id with at least one recorded turn, sorted.
    virtual std::vector<std::string> SessionIds() const = 0;
};

}  // namespace minimal::cost_tracker
