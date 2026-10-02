#pragma once

// The session's pause state: two independent bits packed into one atomic.
//
//   paused        -- new turns and wake-turns are fenced (commands still run).
//   continuation  -- the last turn ended STOP_REASON_PAUSED, so an explicit
//                    unpause has something to continue INTO.
//
// All four combinations are reachable (an unpause racing a still-finishing
// final round leaves "continuation owed, not paused"), so collapsing them would
// drop the recovery path. One atomic rather than two because every transition
// must preserve the OTHER bit: a read-modify-write, not a context-free store.
//
// Reads are lock-free from any thread (the loop's mid-turn pause probe,
// paused() on the reader). Every WRITE must hold the owner's mutex -- the
// transitions are read-modify-writes and this class does not make them atomic
// against each other.

#include <atomic>
#include <cstdint>

namespace minimal::agent {

class PauseGate {
public:
    bool paused() const { return IsPaused(state_.load(std::memory_order_acquire)); }
    bool continuation_pending() const {
        return HasContinuation(state_.load(std::memory_order_acquire));
    }

    // Sets the paused bit, keeping the continuation bit. False when it already
    // had that value (nothing changed).
    bool SetPaused(bool paused) {
        const State cur = state_.load(std::memory_order_relaxed);
        if (IsPaused(cur) == paused) return false;
        Store(paused, HasContinuation(cur));
        return true;
    }
    void ClearPaused() {
        const State cur = state_.load(std::memory_order_relaxed);
        if (IsPaused(cur)) Store(false, HasContinuation(cur));
    }
    void ClearContinuation() {
        const State cur = state_.load(std::memory_order_relaxed);
        if (HasContinuation(cur)) Store(IsPaused(cur), false);
    }
    void MarkContinuationPending() {
        Store(IsPaused(state_.load(std::memory_order_relaxed)), true);
    }

private:
    enum class State : std::uint8_t {
        kNotPaused,
        kPaused,
        kPausedWithContinuation,
        kContinuationPending,
    };
    static constexpr bool IsPaused(State s) {
        return s == State::kPaused || s == State::kPausedWithContinuation;
    }
    static constexpr bool HasContinuation(State s) {
        return s == State::kPausedWithContinuation || s == State::kContinuationPending;
    }
    void Store(bool paused, bool continuation) {
        const State next = paused ? (continuation ? State::kPausedWithContinuation : State::kPaused)
                                  : (continuation ? State::kContinuationPending
                                                  : State::kNotPaused);
        state_.store(next, std::memory_order_release);
    }

    std::atomic<State> state_{State::kNotPaused};
};

}  // namespace minimal::agent
