#pragma once

// Mid-turn provisional usage, keyed by session -- the "live" half of the
// status-line totals. The persistent CostStore is turn-granular ON PURPOSE
// (COST IS DERIVED: the observer's OnTurnCompleted is the sanctioned
// write-once summary), but a turn is many provider rounds, and a round can
// take seconds -- or minutes, when a spawned subagent is doing the work.
// OnProviderRound feeds the rounds here as they land, so the totals RPC can
// show usage while the turn is still running.
//
// LIFETIME IS EXACTLY ONE TURN. OnTurnCompleted clears the session's entry:
// the authoritative record has just taken over, and keeping both would
// double-count. That also makes this state inherently transient -- it dies
// with the process, never reaches storage, and is not rewind-relevant (the
// rewind path only ever reshapes the store).
//
// THREADING. The observer thread writes; the totals RPC reads from gRPC
// threads. One mutex -- the data is a few small structs, contention is
// negligible, and the observer contract ("never wedge a turn") is honored by
// the lock being held for microseconds.

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "cost_model.hpp"
#include "minimal/agent/session_observer.hpp"

namespace minimal::cost_tracker {

class InFlightUsage {
public:
    // Adds one provider round to the session's provisional total. An
    // estimated round (the zero-usage proxy gap) contributes NO tokens but
    // marks the session's provisional sum not-fully-reported -- the same
    // honesty rule the turn record applies, so a live total with an
    // unreported round in it renders "cost?", never a partial figure
    // dressed up as whole.
    void RecordRound(const std::string& session_id, const agent::RoundUsage& round);

    // Drops the session's provisional total. Called on turn completion (the
    // authoritative record replaces it) and on session end.
    void Clear(const std::string& session_id);

    // The session's provisional rounds as synthetic TurnRecords, ready to
    // merge into Aggregate()'s input. Model-keyed: a mid-turn /model switch
    // keeps each round priced at its own rate. usage_reported follows the
    // fully-reported rule above; seq bounds carry the -1 sentinel (this
    // state is never stored, so the rewind classifier never sees it).
    std::vector<TurnRecord> Records(const std::string& session_id) const;

private:
    struct ModelUsage {
        llm::Usage usage;
        bool fully_reported = true;
    };
    mutable std::mutex mtx_;
    std::map<std::string, std::map<std::string, ModelUsage>> sessions_;
};

}  // namespace minimal::cost_tracker
