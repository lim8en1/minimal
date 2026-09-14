#pragma once

// Pushes every completed turn into the cost history, and every provider
// round into the in-flight (mid-turn) provisional total. The store is the
// authoritative ledger (written once per turn); in_flight.hpp is the live
// view the totals RPC merges on top so a RUNNING turn -- subagent runs
// included -- is visible before OnTurnCompleted lands. /cost, the .rpc
// service and the tracking service are read-only over the same store, so
// totals can never disagree with what was recorded.
//
// Observer contract (ARCHITECTURE.md "SessionObserver"): returns void,
// fire-and-forget, dispatched on the single ordered observer thread. The
// store's read-modify-write is serialized by PluginStorage's mutex, and
// dispatch ordering means a session's turns append in turn order and its
// rounds arrive before the turn's completion. InFlightUsage carries its own
// lock for the RPC-side readers; the observer thread never contends with
// itself.

#include "cost_store.hpp"
#include "in_flight.hpp"
#include "minimal/agent/session_observer.hpp"

namespace minimal::cost_tracker {

class CostSessionObserver : public agent::SessionObserver {
public:
    // Borrowed; process lifetime (plugins are never dlclosed).
    CostSessionObserver(CostStore& store, InFlightUsage& in_flight)
        : store_(store), in_flight_(in_flight) {}

    // A failed turn arrives with StopReason::Unknown and zero usage -- which
    // is exactly the zero-usage shape, so it records as unpriced rather than
    // vanishing: the turn happened, its cost is simply unknown.
    void OnTurnCompleted(const agent::SessionInfo& info,
                         const agent::TurnSummary& summary) override;

    // Feeds the live view: one provider round answered inside a still-running
    // turn. The store is deliberately NOT touched here -- turn-granular
    // writes keep the ledger rewind-mappable; rounds live only in the
    // provisional in-flight total.
    void OnProviderRound(const agent::SessionInfo& info,
                         const agent::RoundUsage& round) override;

    // Drops the records the rewind discarded, so a retried turn is not counted
    // twice. Redo (truncated_to_seq < 0) is a no-op: which records the restored
    // tail maps to is unknowable here.
    void OnSessionRewound(const agent::SessionInfo& info,
                          std::int64_t truncated_to_seq) override;

    // A session cannot end mid-turn (the turn always completes or fails
    // first, either path clearing the provisional total), but belt-and-braces:
    // nothing in-flight may outlive its session.
    void OnSessionEnded(const agent::SessionInfo& info) override;

private:
    CostStore& store_;
    InFlightUsage& in_flight_;
};

}  // namespace minimal::cost_tracker
