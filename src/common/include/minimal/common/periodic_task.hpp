#pragma once

// One thread that calls `tick` every `interval()`, until stopped.
//
// `interval` is asked before every wait, so a caller whose period depends on
// runtime configuration (the session reaper's attach guard) picks up a change
// on the next cycle. Stop() wakes the wait immediately and joins; a tick in
// progress completes first. The tick runs with no lock of this class held.
//
// A throwing `tick` or `interval` is logged and the loop carries on (with the
// last good interval); it never takes the process down. The task must not be
// destroyed from inside its own tick: the thread cannot join itself.

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

#include "minimal/common/logger.hpp"
#include "minimal/common/policy.hpp"

namespace minimal::common {

class PeriodicTask {
public:
    PeriodicTask(std::function<std::chrono::milliseconds()> interval, std::function<void()> tick)
        : interval_(std::move(interval)), tick_(std::move(tick)), thread_([this] { Run(); }) {}
    ~PeriodicTask() {
        assert(thread_.get_id() != std::this_thread::get_id() &&
               "PeriodicTask destroyed from its own tick");
        Stop();
    }

    PeriodicTask(const PeriodicTask&) = delete;
    PeriodicTask& operator=(const PeriodicTask&) = delete;

    // Idempotent. From inside a tick it only signals; the destructor joins.
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
        std::chrono::milliseconds interval = kPeriodicTaskFallbackInterval;
        while (true) {
            Guarded("interval", [&] { interval = interval_(); });
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait_for(lock, interval, [this] { return stopping_; });
                if (stopping_) return;
            }
            Guarded("tick", tick_);
        }
    }

    template <typename F>
    static void Guarded(const char* what, F&& f) {
        try {
            f();
        } catch (const std::exception& error) {
            DefaultLogger().warn("periodic task {} threw: {}", what, error.what());
        } catch (...) {
            DefaultLogger().warn("periodic task {} threw a non-standard exception", what);
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
