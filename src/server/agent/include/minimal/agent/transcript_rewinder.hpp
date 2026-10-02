#pragma once

// Rewind / redo of the ACTIVE transcript, store first and then the loop's
// working set. Pure of Session: it reports what happened and the caller emits
// the wire event and notifies observers. Worker thread only (it mutates the
// loop's history).
//
// Store first, then memory: a crash between the two leaves the durable active
// set ahead of the working set, which the resume path loads as the truncated
// transcript -- the state the user asked for. The reverse order would leave
// memory describing turns the store still counts.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "minimal/agent/agent_loop.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/session/history_store.hpp"
#include "minimal/session/session_reader.hpp"

namespace minimal::agent {

// A rewind target must be a REAL user turn: role User carrying text. This
// excludes injected subagent completions (User role, tool-result blocks only)
// and anything else a human never typed.
bool IsRewindTarget(const session::StoredMessage& stored);

struct RewindOutcome {
    // Set => nothing was changed (or, for a thrown failure, the caller turns the
    // exception into a refusal); `truncated_to_seq`/`active_last_seq` are then
    // meaningless.
    std::optional<std::string> refusal;
    std::int64_t truncated_to_seq = -1;  // -1 for a redo
    std::int64_t active_last_seq = -1;

    static RewindOutcome Refused(std::string reason) {
        RewindOutcome outcome;
        outcome.refusal = std::move(reason);
        return outcome;
    }
};

class TranscriptRewinder {
public:
    // Every reference outlives the rewinder (Session owns all of them).
    TranscriptRewinder(std::string session_id, session::HistoryStore& store,
                       const session::SessionReader& reader, AgentLoop& loop,
                       common::Logger& logger)
        : session_id_(std::move(session_id)),
          store_(store),
          reader_(reader),
          loop_(loop),
          logger_(logger) {}

    // Cuts the active transcript before `target_seq`.
    RewindOutcome Rewind(std::int64_t target_seq);
    // Restores the tail the most recent rewind discarded.
    RewindOutcome Redo();

private:
    std::string session_id_;
    session::HistoryStore& store_;
    const session::SessionReader& reader_;
    AgentLoop& loop_;
    common::Logger& logger_;
};

}  // namespace minimal::agent
