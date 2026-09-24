#pragma once

// One thread that calls `tick` every `interval()`, until stopped.
//
// `interval` is asked before every wait, so a caller whose period depends on
// runtime configuration (the session reaper's attach guard) picks up a change
// on the next cycle. Stop() wakes the wait immediately and joins; a tick in
// progress completes first. The tick runs with no lock of this class held.

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace minimal::common {

class PeriodicTask {
public:
    PeriodicTask(std::function<std::chrono::milliseconds()> interval, std::function<void()> tick)
        : interval_(std::move(interval)), tick_(std::move(tick)), thread_([this] { Run(); }) {}
    ~PeriodicTask() { Stop(); }

    PeriodicTask(const PeriodicTask&) = delete;
    PeriodicTask& operator=(const PeriodicTask&) = delete;

    // Idempotent.
    void Stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) thread_.join();
    }

private:
    void Run() {
        while (true) {
            const auto interval = interval_();
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait_for(lock, interval, [this] { return stopping_; });
                if (stopping_) return;
            }
            tick_();
        }
    }

    std::function<std::chrono::milliseconds()> interval_;
    std::function<void()> tick_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stopping_ = false;
    std::thread thread_;  // last: starts running once everything above exists
};

}  // namespace minimal::common
