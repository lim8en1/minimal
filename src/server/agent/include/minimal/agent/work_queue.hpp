#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "minimal/commands/command_context.hpp"
#include "minimal/commands/command_dispatcher.hpp"

namespace minimal::agent {

// One item of work for the session worker. A variant rather than two queues,
// because the ORDER between a turn and a command matters and two queues have no
// order between them -- `/model x` typed before a turn must apply before it.
struct QueuedCommand {
    std::string request_id;  // echoed on CommandResult
    std::string name;
    commands::CommandArgs args;
};

// One traversal request for the session worker. A rewind cuts the ACTIVE
// transcript before target_seq (the target must be an active, text-bearing
// user message); a redo restores the tail the most recent rewind discarded.
// See ARCHITECTURE.md "Session traversal" and the RewindRequest proto comment
// for the mark/fork/reclaim model.
struct QueuedRewind {
    std::int64_t target_seq = -1;  // meaningful only when redo == false
    bool redo = false;
};

// One client-driven run-mode change (shift+tab) is NOT a work item: like
// pause (SetPausedEvent), it is applied on the reader thread by flipping the
// executor's atomic. It was queued to the worker once, on the theory that
// SetRunMode was worker-thread-only -- but the state is a single atomic enum
// whose reads are lock-free from any thread, so the queue bought a
// turn-boundary delay and no safety. See Session::SetRunMode.

// The explicit-resume kick (SetPausedEvent{paused=false} with a turn that
// ended STOP_REASON_PAUSED and nothing else queued). Runs the seedless wake
// turn: the model continues from the persisted tool results the paused turn
// left behind -- possibly answering a steered message that flushed after it.
// Enqueued only when no turn is running or queued, so it can never double-run
// behind a real turn; a turn that was merely requeued by auto-resume clears
// the pending continuation at pop instead.
struct QueuedResume {};

// A user turn carrying PER-TURN model/agent overrides. Applied for this turn
// only, on the worker at turn pop -- the sticky session preset, the session's
// model_override_, and every metadata event are untouched. The model rides as
// its RAW "<provider>/<model>" string and is split and resolved at pop: an
// unknown or malformed ref fails the turn with a named error, never a silent
// fall-back, and the reader never blocks on resolution.
struct QueuedTurn {
    std::string text;
    std::string model;         // raw "<provider>/<model>"; empty = keep the session's
    std::string agent_preset;  // empty = keep the session's own
    // Server-assigned at submit (per-session monotonic), announced to every
    // subscriber by TurnQueuedEvent: what lets a client cancel THIS queued
    // turn before it runs, and correlate a cancel with its optimistic echo.
    std::uint64_t turn_id = 0;
};

using WorkItem = std::variant<QueuedTurn, QueuedCommand, QueuedRewind, QueuedResume>;

// The session worker's queue: ONE ordered, bounded queue for turns, commands
// and rewinds (two queues would have no order between them, so `/model x` typed
// before a turn could apply after it).
//
// The bound is a ceiling against a runaway producer, not a tuning knob: one
// turn can run for minutes, so a client streaming turns could otherwise enqueue
// faster than they are consumed and grow memory without limit. Beyond it the
// submit is refused (TryPush false) and the caller answers on the wire.
//
// NOT thread-safe: the owner guards every call with its own mutex.
class WorkQueue {
public:
    static constexpr std::size_t kMaxItems = 32;

    bool empty() const { return items_.empty(); }
    std::size_t size() const { return items_.size(); }
    bool full() const { return items_.size() >= kMaxItems; }

    // False (nothing queued) when full.
    bool TryPush(WorkItem item) {
        if (full()) return false;
        items_.push_back(std::move(item));
        return true;
    }

    // PAUSE HOLD: while paused only non-turn work may run -- commands, rewinds,
    // the resume kick -- so /agent, /model and a rewind all execute against a
    // paused session while its turns wait IN PLACE (not reordered around).
    bool HasRunnable(bool paused) const {
        if (items_.empty()) return false;
        if (!paused) return true;
        for (const auto& item : items_) {
            if (!std::holds_alternative<QueuedTurn>(item)) return true;
        }
        return false;
    }

    // The next runnable item: the front when not paused, else the first
    // non-turn item (held turns stay where they are). nullopt when none.
    std::optional<WorkItem> PopRunnable(bool paused) {
        for (auto it = items_.begin(); it != items_.end(); ++it) {
            if (paused && std::holds_alternative<QueuedTurn>(*it)) continue;
            WorkItem item = std::move(*it);
            items_.erase(it);
            return item;
        }
        return std::nullopt;
    }

    // Removes the queued turn with this id; false when it is not queued.
    bool RemoveTurn(std::uint64_t turn_id) {
        for (auto it = items_.begin(); it != items_.end(); ++it) {
            const auto* turn = std::get_if<QueuedTurn>(&*it);
            if (turn == nullptr || turn->turn_id != turn_id) continue;
            items_.erase(it);
            return true;
        }
        return false;
    }

    // Removes every queued TURN, returning their ids in queue order. Commands
    // and rewinds stay: a queued /model the user asked for one submission ago
    // is not silently undone by cancelling the queue's messages.
    std::vector<std::uint64_t> RemoveAllTurns() {
        std::vector<std::uint64_t> dropped;
        for (auto it = items_.begin(); it != items_.end();) {
            if (const auto* turn = std::get_if<QueuedTurn>(&*it)) {
                dropped.push_back(turn->turn_id);
                it = items_.erase(it);
            } else {
                ++it;
            }
        }
        return dropped;
    }

private:
    std::deque<WorkItem> items_;
};

}  // namespace minimal::agent
