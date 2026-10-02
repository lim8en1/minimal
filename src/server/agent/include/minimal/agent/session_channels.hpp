#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>

#include "minimal/agent/injection_queue.hpp"

namespace minimal::agent {

// Session's domain-only cross-thread state -- the half of what this block used
// to bundle that has nothing to do with the wire. The transport half
// (EventBroadcaster, PendingRequests) split out to rpc::TransportChannels
// (see rpc/transport_channels.hpp) once Session stopped touching either
// directly; this block keeps existing, by shared_ptr, for exactly the same
// reason it always did -- background watcher threads capture it and may
// outlive both the worker and Session itself.
struct SessionChannels {
    // Live background tasks: STARTED increments, a terminal state decrements.
    // Lives HERE, on the shared_ptr block, because the task_event_sink that
    // maintains it is captured by background watchers that may fire after the
    // Session is gone.
    std::atomic<std::size_t> background_tasks_live{0};
    // Cross-thread staging for out-of-band messages (non-blocking subagent
    // completions, background task results), drained at turn boundaries. Lives
    // HERE -- on the shared_ptr block -- because that is exactly the object a
    // cross-thread handle must outlive in order to be safe: a tool's captured
    // schedule_injection holds the channels shared_ptr, so the queue (and the
    // callables' state) survive past the session and Push refuses after
    // Terminate closes it. See injection_queue.hpp.
    InjectionQueue injections;

    // Wakes the session's worker after an injection landed from a foreign
    // thread (a background task's watcher): the wait predicate only re-reads the
    // queue when notified. Set by the Session, cleared before it is destroyed --
    // the mutex makes "call it" and "clear it" mutually exclusive, so a watcher
    // outliving the session never calls into a dead object.
    void SetWake(std::function<void()> wake) {
        std::lock_guard<std::mutex> lock(wake_mutex_);
        wake_ = std::move(wake);
    }
    void ClearWake() { SetWake({}); }
    void Wake() {
        std::lock_guard<std::mutex> lock(wake_mutex_);
        if (wake_) wake_();
    }

private:
    std::mutex wake_mutex_;
    std::function<void()> wake_;
};

inline std::shared_ptr<SessionChannels> MakeSessionChannels() {
    return std::make_shared<SessionChannels>();
}

}  // namespace minimal::agent
