#pragma once

// Holds registered observers and dispatches to them off the session's threads.
//
// Dispatch is FIRE-AND-FORGET and never awaited. It cannot block the session
// worker and cannot delay session teardown. Deliberately NOT "run with a
// timeout": a detached task cannot be interrupted in C++, so a timeout there
// would be decorative.
//
// ONE DISPATCH THREAD, NOT A POOL -- a deliberate divergence from
// ARCHITECTURE.md, which said "thread pool". A pool gives no ordering guarantee
// between tasks, and observer notifications are inherently ordered: an observer
// that keys off OnSessionCreated and releases on OnSessionEnded is broken if
// those can arrive in either order, or concurrently. A pool would also make
// every observer's own state a shared mutable, so writing a correct observer
// would require plugin authors to be concurrency experts. Serialising costs
// nothing real -- observer work is meant to be short -- and buys both ordering
// and the guarantee that an observer never runs concurrently with itself.
//
// The failure mode shifts accordingly: a hung observer stalls FURTHER
// notifications rather than leaking one pool thread. That is contained (sessions
// keep running; only notifications stop) and it is the right trade against
// silently racing plugin state.
//
// CONTAINMENT IS ENFORCED BY TryPush, not merely asserted here. Dispatch used
// Push -- which blocks at the queue's cap -- while NotifySessionCreated runs on
// the gRPC HANDLER thread (inside Session's constructor) and NotifySessionEnded
// runs from Terminate on the reaper. A wedged observer therefore filled the
// queue and then blocked session creation and teardown, which is the opposite of
// the paragraph above. Notifications are dropped and counted instead.
//
// Observers are plugin code, so a throwing observer is expected rather than
// exceptional: each call is wrapped, and one observer's failure never stops the
// others from running.

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "minimal/agent/session_observer.hpp"
#include "minimal/common/event_queue.hpp"
#include "minimal/common/logger.hpp"

namespace minimal::agent {

class SessionObserverRegistry {
public:
    // The queue bound is EXPLICIT rather than EventQueue's default. That default
    // (65536) was sized for stream events -- token deltas -- and a notification
    // task is a different animal: each holds a copy of SessionInfo plus the
    // observer snapshot, so inheriting it reserved tens of megabytes for a
    // backlog that means "an observer has stopped consuming" long before it
    // fills. A few hundred pending notifications is already pathological.
    // Injectable so a test can wedge an observer and reach the bound without
    // queueing 65536 of them.
    static constexpr std::size_t kDefaultTaskQueueSize = 1024;

    explicit SessionObserverRegistry(common::Logger& logger,
                                     std::size_t task_queue_size = kDefaultTaskQueueSize);

    // Notifications refused because the queue was full. Read by tests, and the
    // number the warning reports.
    std::size_t dropped_notifications() const {
        return dropped_.load(std::memory_order_acquire);
    }
    ~SessionObserverRegistry();

    // Drains the queued notifications, joins the dispatcher, then RELEASES
    // every observer (running their destructors). Idempotent; the destructor
    // calls it too. The host calls it explicitly once every session has
    // terminated: observers reach services, the session database, and
    // SessionSupport, which main() declares AFTER this registry and so
    // destroys first -- left to the destructor, the drained backlog and any
    // observer-owned worker would run against already-destroyed objects.
    // Notifications after Shutdown find no observers and are no-ops.
    void Shutdown();

    SessionObserverRegistry(const SessionObserverRegistry&) = delete;
    SessionObserverRegistry& operator=(const SessionObserverRegistry&) = delete;

    // Registration is expected at startup, before any session exists. The
    // registry keeps the observer alive for its own lifetime -- an observer
    // lives in a plugin .so that is never dlclose'd, matching every other
    // registry here.
    void Register(std::shared_ptr<SessionObserver> observer);

    // Attribution window (the loader's shape for every registry): observers
    // registered while it is open belong to `plugin_name`.
    void SetAttribution(std::string plugin_name);
    void ClearAttribution();
    std::size_t CountOwnedBy(const std::string& plugin_name) const;

    // Registration-time rollback for a plugin whose hook threw. Safe ONLY
    // before any session exists -- no dispatched task can still hold a
    // snapshot -- and the observer objects live in .so files that are never
    // dlclosed, so destroying them here cannot dangle.
    void RemoveByAttribution(const std::string& plugin_name);

    void NotifySessionCreated(SessionInfo info);
    void NotifyProviderRound(SessionInfo info, RoundUsage round);
    void NotifyTurnCompleted(SessionInfo info, TurnSummary summary);
    void NotifyToolExecuted(SessionInfo info, ToolInvocation invocation);
    void NotifySessionRewound(SessionInfo info, std::int64_t truncated_to_seq);
    void NotifySessionEnded(SessionInfo info);

    std::size_t observer_count() const;

    // Testing seam. Blocks until every dispatched task has run, which is exactly
    // what production must never do -- so it is spelled as a wait rather than
    // offered as a Flush() someone might reach for in the session path.
    void WaitForIdleForTesting();

private:
    void Dispatch(std::function<void(SessionObserver&)> call);

    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<SessionObserver>> observers_;
    std::vector<std::string> owners_;  // parallel to observers_
    std::string attribution_;
    // Notified whenever outstanding_ reaches zero, under mutex_ -- the
    // decrement itself must happen under the same lock WaitForIdleForTesting
    // waits on, or the transition-to-zero and the wait can race (classic
    // lost wakeup).
    std::condition_variable idle_cv_;

    common::Logger& logger_;
    common::EventQueue<std::function<void()>> tasks_;
    std::thread dispatcher_;
    std::atomic<int> outstanding_{0};
    // Notifications refused because the queue was full or closed. Counted so a
    // plugin missing an OnSessionEnded has evidence other than its own leak.
    std::atomic<std::size_t> dropped_{0};
};

}  // namespace minimal::agent
