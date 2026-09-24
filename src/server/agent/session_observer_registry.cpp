#include "minimal/agent/session_observer_registry.hpp"

#include <algorithm>
#include <exception>
#include <utility>

namespace minimal::agent {

SessionObserverRegistry::SessionObserverRegistry(common::Logger& logger,
                                                 std::size_t task_queue_size)
    : logger_(logger), executor_("session observer", task_queue_size, logger) {}

SessionObserverRegistry::~SessionObserverRegistry() { Shutdown(); }

void SessionObserverRegistry::Shutdown() {
    // Closing drains the backlog first, so observer calls already queued still
    // run -- OnSessionEnded at shutdown is exactly the notification a plugin
    // needs in order to release per-session state.
    executor_.Stop(common::SerialExecutor::Backlog::kRun);
    // Then release the observers, so their destructors (a plugin joining its
    // own worker) run NOW, while whatever the caller keeps alive past this
    // point is still standing. Swapped out under the lock and destroyed
    // outside it: an observer destructor must not run holding mutex_.
    std::vector<std::shared_ptr<SessionObserver>> released;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        released.swap(observers_);
        owners_.clear();
    }
}

void SessionObserverRegistry::Register(std::shared_ptr<SessionObserver> observer) {
    if (observer == nullptr) return;
    std::lock_guard<std::mutex> lock(mutex_);
    observers_.push_back(std::move(observer));
    owners_.push_back(attribution_);
}

void SessionObserverRegistry::SetAttribution(std::string plugin_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    attribution_ = std::move(plugin_name);
}

void SessionObserverRegistry::ClearAttribution() {
    std::lock_guard<std::mutex> lock(mutex_);
    attribution_.clear();
}

std::size_t SessionObserverRegistry::CountOwnedBy(const std::string& plugin_name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<std::size_t>(std::count(owners_.begin(), owners_.end(), plugin_name));
}

void SessionObserverRegistry::RemoveByAttribution(const std::string& plugin_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (std::size_t i = observers_.size(); i-- > 0;) {
        if (owners_[i] != plugin_name) continue;
        observers_.erase(observers_.begin() + static_cast<std::ptrdiff_t>(i));
        owners_.erase(owners_.begin() + static_cast<std::ptrdiff_t>(i));
    }
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

    // TryPost, NEVER a blocking post, and the containment property this class advertises is
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
    if (!executor_.TryPost([this, snapshot = std::move(snapshot), call = std::move(call)] {
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
        // Refused: the queue is full (a wedged observer) or already stopped
        // (shutdown).
        const auto n = dropped_.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (!executor_.stopped()) {
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

void SessionObserverRegistry::WaitForIdleForTesting() { executor_.WaitForIdle(); }

}  // namespace minimal::agent
