#include "minimal/rpc/command_executor.hpp"

#include <algorithm>
#include <vector>

#include "minimal/common/logger.hpp"
#include "minimal/rpc/policy.hpp"

namespace minimal::rpc {

CommandExecutor::CommandExecutor(std::size_t threads, std::size_t max_backlog)
    : CommandExecutor(threads, max_backlog, Options{}) {}

CommandExecutor::CommandExecutor(std::size_t threads, std::size_t max_backlog, Options options)
    : options_(options), max_backlog_(max_backlog) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (std::size_t i = 0; i < threads; ++i) SpawnWorkerLocked();
    }
    if (options_.deadline.count() > 0) watchdog_ = std::thread([this] { WatchdogMain(); });
}

CommandExecutor::~CommandExecutor() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    watchdog_cv_.notify_all();
    if (watchdog_.joinable()) watchdog_.join();
    // The watchdog is gone, so no worker is spawned from here on. A stuck task
    // still blocks this join -- the pre-existing shutdown contract (see the
    // header): tasks must be able to finish.
    std::list<std::thread> workers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        workers.swap(workers_);
        workers.splice(workers.end(), finished_);
    }
    for (auto& worker : workers) {
        if (worker.joinable()) worker.join();
    }
}

void CommandExecutor::JoinFinished() {
    std::list<std::thread> finished;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        finished.swap(finished_);
    }
    // Each has left its loop and is only returning: the join is immediate.
    for (auto& thread : finished) {
        if (thread.joinable()) thread.join();
    }
}

void CommandExecutor::SpawnWorkerLocked() {
    workers_.emplace_back([this] { WorkerMain(); });
}

void CommandExecutor::ReleaseKeyLocked(const std::string& key) {
    if (key.empty()) return;
    const auto it = per_key_.find(key);
    if (it == per_key_.end()) return;
    if (--it->second == 0) per_key_.erase(it);
}

bool CommandExecutor::TryPost(Task task, std::string key, Task on_timeout) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || items_.size() >= max_backlog_) return false;
        if (abandoned_ >= options_.max_abandoned) return false;
        if (!key.empty()) {
            std::size_t& held = per_key_[key];
            if (held >= options_.max_per_key) return false;
            ++held;
        }
        items_.push_back(Item{std::move(task), std::move(key), std::move(on_timeout)});
    }
    cv_.notify_one();
    return true;
}

void CommandExecutor::WorkerMain() {
    while (true) {
        Item item;
        std::list<Running>::iterator slot;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stopping_ || !items_.empty(); });
            if (items_.empty()) return;  // stopping_ && drained
            item = std::move(items_.front());
            items_.pop_front();
            running_.push_back(Running{std::chrono::steady_clock::now(), item.key,
                                       std::move(item.on_timeout), false});
            slot = std::prev(running_.end());
        }
        // Catch-all, not just std::exception: a task that throws a
        // plugin-originated non-standard exception must not terminate the
        // executor's thread -- the pool outlives any single task.
        try {
            item.task();
        } catch (const std::exception& e) {
            // Reaching here means the task's own containment missed something.
            common::DefaultLogger().warn("command pool task threw: {}", e.what());
        } catch (...) {
            common::DefaultLogger().warn("command pool task threw a non-standard exception");
        }
        bool replaced = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            replaced = slot->timed_out;
            if (replaced) {
                // The watchdog already released the key and spawned this
                // thread's replacement; this one just stops being "stuck".
                --abandoned_;
            } else {
                ReleaseKeyLocked(slot->key);
            }
            running_.erase(slot);
            if (replaced) {
                // Its replacement already runs; hand this thread's handle to
                // the watchdog to join, or every timeout would leave one
                // unjoined (stack and all) until shutdown.
                const auto self = std::this_thread::get_id();
                for (auto it = workers_.begin(); it != workers_.end(); ++it) {
                    if (it->get_id() == self) {
                        finished_.splice(finished_.end(), workers_, it);
                        break;
                    }
                }
            }
        }
        if (replaced) return;
    }
}

void CommandExecutor::WatchdogMain() {
    const auto tick = std::min(options_.deadline / kWatchdogTicksPerDeadline, kMaxWatchdogTick);
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopping_) {
        watchdog_cv_.wait_for(lock, std::max(tick, kMinWatchdogTick),
                              [this] { return stopping_; });
        if (stopping_) break;
        if (!finished_.empty()) {
            lock.unlock();
            JoinFinished();
            lock.lock();
            if (stopping_) break;
        }
        const auto now = std::chrono::steady_clock::now();
        std::vector<Task> notify;
        for (auto& running : running_) {
            if (running.timed_out || now - running.started < options_.deadline) continue;
            running.timed_out = true;
            ++abandoned_;
            ReleaseKeyLocked(running.key);
            SpawnWorkerLocked();  // capacity returns while the stuck thread finishes
            if (running.on_timeout) notify.push_back(std::move(running.on_timeout));
        }
        if (notify.empty()) continue;
        lock.unlock();
        for (auto& callback : notify) {
            try {
                callback();
            } catch (...) {
                common::DefaultLogger().warn("command pool on_timeout callback threw");
            }
        }
        lock.lock();
    }
}

}  // namespace minimal::rpc
