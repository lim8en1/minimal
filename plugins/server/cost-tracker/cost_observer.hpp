#pragma once

// Pushes every completed turn into the cost history. The one seam that
// WRITES: /cost, the .rpc service and the tracking service are read-only over
// the same store, so totals can never disagree with what was recorded.
//
// Observer contract (ARCHITECTURE.md "SessionObserver"): returns void,
// fire-and-forget, dispatched on the single ordered observer thread. The
// store's read-modify-write is serialized by PluginStorage's mutex, and
// dispatch ordering means a session's turns append in turn order -- no lock
// of this plugin's own, and no possibility of racing itself.

#include "cost_store.hpp"
#include "minimal/agent/session_observer.hpp"

namespace minimal::cost_tracker {

class CostSessionObserver : public agent::SessionObserver {
public:
    // Borrowed; process lifetime (plugins are never dlclosed).
    explicit CostSessionObserver(CostStore& store) : store_(store) {}

    // A failed turn arrives with StopReason::Unknown and zero usage -- which
    // is exactly the zero-usage shape, so it records as unpriced rather than
    // vanishing: the turn happened, its cost is simply unknown.
    void OnTurnCompleted(const agent::SessionInfo& info,
                         const agent::TurnSummary& summary) override;

private:
    CostStore& store_;
};

}  // namespace minimal::cost_tracker
