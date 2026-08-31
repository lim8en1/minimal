#pragma once

// The concrete provider behind the shipped CostTrackingService interface.
// Read-only over the shared store; every other seam of this plugin (observer,
// /cost, .rpc) reads or writes the same state, so there is exactly one place
// cost totals can come from.

#include <string>
#include <vector>

#include "cost_model.hpp"
#include "cost_store.hpp"
#include "cost_tracking_service.hpp"

namespace minimal::cost_tracker {

class CostTrackingServiceImpl : public CostTrackingService {
public:
    CostTrackingServiceImpl(CostStore& store, RateTable& rates) : store_(store), rates_(rates) {}

    std::optional<SessionTotals> Totals(const std::string& session_id) const override;
    std::vector<std::string> SessionIds() const override;

private:
    CostStore& store_;
    RateTable& rates_;
};

}  // namespace minimal::cost_tracker
