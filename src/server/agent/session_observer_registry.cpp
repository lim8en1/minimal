#include "minimal/agent/session_observer_registry.hpp"

#include <exception>
#include <utility>

namespace minimal::agent {

SessionObserverRegistry::SessionObserverRegistry(common::Logger& logger) : logger_(logger) {
    dispatcher_ = std::thread([this] {
        // One thread, so tasks run in the order they were queued. That ordering
        // IS the contract: OnSessionCreated must be delivered before the
        // OnSessionEnded for the same session.
        while (auto task = tasks_.Pop()) {
            (*task)();
            outstanding_.fetch_sub(1, std::memory_order_acq_rel);
        }
    });
}

SessionObserverRegistry::~SessionObserverRegistry() {
    // Closing drains the backlog first, so observer calls already queued still
    // run -- OnSessionEnded at shutdown is exactly the notification a plugin
    // needs in order to release per-session state.
    tasks_.Close();
    if (dispatcher_.joinable()) dispatcher_.join();
}

void SessionObserverRegistry::Register(std::shared_ptr<SessionObserver> observer) {
    if (observer == nullptr) return;
    std::lock_guard<std::mutex> lock(mutex_);
    observers_.push_back(std::move(observer));
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
    tasks_.Push([this, snapshot = std::move(snapshot), call = std::move(call)] {
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
    });
}

void SessionObserverRegistry::NotifySessionCreated(SessionInfo info) {
    Dispatch([info = std::move(info)](SessionObserver& observer) {
        observer.OnSessionCreated(info);
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

void SessionObserverRegistry::NotifySessionEnded(SessionInfo info) {
    Dispatch([info = std::move(info)](SessionObserver& observer) {
        observer.OnSessionEnded(info);
    });
}

void SessionObserverRegistry::WaitForIdleForTesting() {
    while (outstanding_.load(std::memory_order_acquire) > 0) {
        std::this_thread::yield();
    }
}

}  // namespace minimal::agent
