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
// Observers are plugin code, so a throwing observer is expected rather than
// exceptional: each call is wrapped, and one observer's failure never stops the
// others from running.

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "minimal/agent/session_observer.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/rpc/event_queue.hpp"

namespace minimal::agent {

class SessionObserverRegistry {
public:
    explicit SessionObserverRegistry(common::Logger& logger);
    ~SessionObserverRegistry();

    SessionObserverRegistry(const SessionObserverRegistry&) = delete;
    SessionObserverRegistry& operator=(const SessionObserverRegistry&) = delete;

    // Registration is expected at startup, before any session exists. The
    // registry keeps the observer alive for its own lifetime -- an observer
    // lives in a plugin .so that is never dlclose'd, matching every other
    // registry here.
    void Register(std::shared_ptr<SessionObserver> observer);

    void NotifySessionCreated(SessionInfo info);
    void NotifyTurnCompleted(SessionInfo info, TurnSummary summary);
    void NotifyToolExecuted(SessionInfo info, ToolInvocation invocation);
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

    common::Logger& logger_;
    rpc::EventQueue<std::function<void()>> tasks_;
    std::thread dispatcher_;
    std::atomic<int> outstanding_{0};
};

}  // namespace minimal::agent
