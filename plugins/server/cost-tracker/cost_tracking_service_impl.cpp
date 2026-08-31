#include "cost_tracking_service_impl.hpp"

namespace minimal::cost_tracker {

std::optional<SessionTotals> CostTrackingServiceImpl::Totals(
    const std::string& session_id) const {
    if (!store_.available()) return std::nullopt;
    try {
        const auto history = store_.Load(session_id);
        if (history.turns.empty()) return std::nullopt;
        return Aggregate(session_id, history.parent_session_id, history.turns,
                         [this](const std::string& model) { return rates_.Resolve(model); });
    } catch (const std::exception&) {
        // A service is a runtime condition, not an error path (the
        // ServiceRegistry contract): a corrupt blob reads as no data.
        return std::nullopt;
    }
}

std::vector<std::string> CostTrackingServiceImpl::SessionIds() const {
    if (!store_.available()) return {};
    return store_.SessionIds();
}

}  // namespace minimal::cost_tracker
