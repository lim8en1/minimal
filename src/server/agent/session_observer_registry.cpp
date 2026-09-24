#include "minimal/agent/session_observer_registry.hpp"

#include <exception>
#include <utility>

namespace minimal::agent {

SessionObserverRegistry::SessionObserverRegistry(common::Logger& logger,
                                                 std::size_t task_queue_size)
    : logger_(logger), tasks_(task_queue_size) {
    dispatcher_ = std::thread([this] {
        // One thread, so tasks run in the order they were queued. That ordering
        // IS the contract: OnSessionCreated must be delivered before the
        // OnSessionEnded for the same session.
        while (auto task = tasks_.Pop()) {
            (*task)();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                outstanding_.fetch_sub(1, std::memory_order_acq_rel);
            }
            idle_cv_.notify_all();
        }
    });
}

SessionObserverRegistry::~SessionObserverRegistry() { Shutdown(); }

void SessionObserverRegistry::Shutdown() {
    // Closing drains the backlog first, so observer calls already queued still
    // run -- OnSessionEnded at shutdown is exactly the notification a plugin
    // needs in order to release per-session state.
    tasks_.Close();
    if (dispatcher_.joinable()) dispatcher_.join();
    // Then release the observers, so their destructors (a plugin joining its
    // own worker) run NOW, while whatever the caller keeps alive past this
    // point is still standing. Swapped out under the lock and destroyed
    // outside it: an observer destructor must not run holding mutex_.
    std::vector<std::shared_ptr<SessionObserver>> released;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        released.swap(observers_);
    }
}

void SessionObserverRegistry::Register(std::shared_ptr<SessionObserver> observer) {
    if (observer == nullptr) return;
    std::lock_guard<std::mutex> lock(mutex_);
    observers_.push_back(std::move(observer));
}

void SessionObserverRegistry::RemoveAfter(std::size_t existing_count) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (existing_count < observers_.size()) observers_.resize(existing_count);
}

std::size_t SessionObserverRegistry::observer_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return observers_.size();
}

void SessionObserverRegistry::Dispatch(std::function<void(SessionObserver&)> call) {
    std::vector<std::shared_ptr<SessionObserver>> snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (observers_.empty()) return;
        snapshot = observers_;
    }

    outstanding_.fetch_add(1, std::memory_order_acq_rel);
    // TryPush, NEVER Push, and the containment property this class advertises is
    // the reason. Push blocks at the queue's cap, and the threads that dispatch
    // are not allowed to block: NotifySessionCreated runs inside Session's
    // constructor, which SessionRegistry::CreateResolved calls from the gRPC
    // HANDLER thread, and NotifySessionEnded runs from Session::Terminate on the
    // reaper and in ~SessionRegistry. So a wedged observer -- the documented
    // failure mode, "sessions keep running; only notifications stop" -- would
    // fill the queue and then block session creation and teardown outright,
    // including the one thread the three-thread topology forbids from blocking.
    //
    // Dropping a notification to a consumer that has stopped consuming is the
    // correct degradation; stalling the transport is not. Counted rather than
    // silent, because a plugin that misses an OnSessionEnded leaks its
    // per-session state and the only evidence would otherwise be the leak.
    if (!tasks_.TryPush([this, snapshot = std::move(snapshot), call = std::move(call)] {
        for (const auto& observer : snapshot) {
            // Observers are plugin code, so a throw is expected rather than
            // exceptional. One misbehaving observer must not stop the others --
            // and must not propagate into the pool thread, which would take down
            // every future notification with it.
            try {
                call(*observer);
            } catch (const std::exception& e) {
                logger_.warn("session observer threw: {}", e.what());
            } catch (...) {
                logger_.warn("session observer threw a non-std exception");
            }
        }
    })) {
        // Refused: the queue is full (a wedged observer) or already closed
        // (shutdown). Undo the outstanding count the task will never decrement,
        // or WaitForIdleForTesting waits forever on a task that does not exist.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            outstanding_.fetch_sub(1, std::memory_order_acq_rel);
        }
        idle_cv_.notify_all();
        const auto n = dropped_.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (!tasks_.closed()) {
            logger_.warn("session observer notification dropped (queue full); {} dropped so far",
                         n);
        }
    }
}

void SessionObserverRegistry::NotifySessionCreated(SessionInfo info) {
    Dispatch([info = std::move(info)](SessionObserver& observer) {
        observer.OnSessionCreated(info);
    });
}

void SessionObserverRegistry::NotifyProviderRound(SessionInfo info, RoundUsage round) {
    Dispatch([info = std::move(info), round = std::move(round)](SessionObserver& observer) {
        observer.OnProviderRound(info, round);
    });
}

void SessionObserverRegistry::NotifyTurnCompleted(SessionInfo info, TurnSummary summary) {
    Dispatch([info = std::move(info), summary = std::move(summary)](SessionObserver& observer) {
        observer.OnTurnCompleted(info, summary);
    });
}

void SessionObserverRegistry::NotifyToolExecuted(SessionInfo info, ToolInvocation invocation) {
    Dispatch(
        [info = std::move(info), invocation = std::move(invocation)](SessionObserver& observer) {
            observer.OnToolExecuted(info, invocation);
        });
}

void SessionObserverRegistry::NotifySessionRewound(SessionInfo info,
                                                   std::int64_t truncated_to_seq) {
    Dispatch([info = std::move(info), truncated_to_seq](SessionObserver& observer) {
        observer.OnSessionRewound(info, truncated_to_seq);
    });
}

void SessionObserverRegistry::NotifySessionEnded(SessionInfo info) {
    Dispatch([info = std::move(info)](SessionObserver& observer) {
        observer.OnSessionEnded(info);
    });
}

void SessionObserverRegistry::WaitForIdleForTesting() {
    std::unique_lock<std::mutex> lock(mutex_);
    idle_cv_.wait(lock, [this] { return outstanding_.load(std::memory_order_acquire) == 0; });
}

}  // namespace minimal::agent
