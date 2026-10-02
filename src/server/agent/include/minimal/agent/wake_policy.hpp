#pragma once

// When should a parked session worker stop waiting? One pure function over a
// snapshot of the session's state, so every rule -- and the order they apply in
// -- is stated in one place and table-testable, instead of living inside a
// condition-variable lambda.
//
// The snapshot is taken under the session mutex. Its fields are cheap to read
// (queue emptiness, an O(1) check of the history's last message), so they are
// gathered eagerly rather than lazily.

namespace minimal::agent {

struct WakeInputs {
    bool terminated = false;
    // The work queue.
    bool queue_empty = true;
    bool queue_has_runnable = false;  // WorkQueue::HasRunnable(paused)
    bool paused = false;
    bool subagent_finished = false;
    // Out-of-band injections (background results, subagent completions).
    bool wake_on_injection = false;
    bool worker_idle = true;
    bool injections_pending = false;
    bool injections_have_user_text = false;
    // The history ends in unanswered user text AND the last turn ended with a
    // natural model stop (the steering sweep's gate).
    bool unanswered_after_natural_turn = false;
    bool continuation_pending = false;
};

inline bool WorkerShouldProceed(const WakeInputs& in) {
    if (in.terminated) return true;
    if (!in.queue_empty) {
        // Queued work: run it unless the pause holds every item (turns only).
        // Queued-but-held work never falls through to the wake paths -- a
        // paused session makes no provider call nobody asked for.
        return in.queue_has_runnable;
    }
    // Idle: the wake paths. All of them hold while paused.
    if (in.paused) return false;
    // A finished subagent runs nothing more (the worker drops every wake
    // silently); without this, steered text left in the queue keeps the
    // predicate true and the worker spins.
    if (in.subagent_finished) return false;
    // An out-of-band message wakes an idle worker only when the setting says so
    // -- a wake is a real provider call the user did not ask for.
    if (in.wake_on_injection && in.worker_idle && in.injections_pending) return true;
    // Real-time steering sweep: a steering message that missed its turn's flush
    // is a human prompt sitting unanswered. It wakes the worker REGARDLESS of
    // wake_on_injection -- a user message is explicit intent, which a
    // background result is not. Keyed on the HISTORY (the durable record) as
    // well as the queue: the turn's final flush may already have drained the
    // queue and appended the message unanswered.
    if (in.injections_have_user_text || in.unanswered_after_natural_turn) return true;
    // The explicit-resume kick raced a still-running final round: the turn then
    // ended Paused, the continuation survived, and this is what turns it into
    // the actual continuation.
    return in.continuation_pending;
}

}  // namespace minimal::agent
