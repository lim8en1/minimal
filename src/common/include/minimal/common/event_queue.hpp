#pragma once

// A bounded, thread-safe FIFO: many producers, one draining consumer.
//
// Users: SerialExecutor (its task queue) and the session-titler plugin (its
// job queue). The server's outbound event stream does NOT use it -- that is
// EventBroadcaster, a never-blocking drop-oldest design with its own byte
// accounting.
//
// BOUNDED by count: buffering is deliberate (a producer that starts before
// anyone is draining should not have to know that, and should not lose work
// for it), but unbounded buffering behind a stalled consumer is a leak.
// Backpressure rather than dropping, because a hole in the middle of ordered
// work is worse than the producer stalling.
//
// Two push flavours: Push blocks while full and is for a thread allowed to
// block; TryPush never blocks and is for a thread that must not.
//
// Templated so the ordering and concurrency properties are tested against a
// trivial payload type.

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>
#include "minimal/common/policy.hpp"

namespace minimal::common {

template <typename T>
class EventQueue {
public:
    explicit EventQueue(std::size_t max_size = kDefaultEventQueueMaxSize) : max_size_(max_size) {}

    // Blocks while the queue is full. Returns whether the event was queued:
    // false means the queue is (or became, while waiting) closed.
    bool Push(T event) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            space_cv_.wait(lock, [this] { return closed_ || events_.size() < max_size_; });
            if (closed_) return false;
            events_.push_back(std::move(event));
        }
        cv_.notify_one();
        return true;
    }

    // Never blocks. False means the queue refused the event -- full or
    // closed -- and the caller decides what a refusal means.
    bool TryPush(T event) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_ || events_.size() >= max_size_) return false;
            events_.push_back(std::move(event));
        }
        cv_.notify_one();
        return true;
    }

    // The single consumer. Blocks until an event is available or the queue is
    // closed AND drained; nullopt means "no more events, ever". Draining
    // before honouring the close is deliberate: work queued before teardown
    // still runs unless the owner explicitly calls DiscardPending.
    std::optional<T> Pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return !events_.empty() || closed_; });
        return TakeFrontLocked();
    }

    // Non-blocking drain, for a consumer that also has other work to poll.
    std::optional<T> TryPop() {
        std::lock_guard<std::mutex> lock(mutex_);
        return TakeFrontLocked();
    }

    // Idempotent. Wakes a blocked consumer AND any blocked producer; queued
    // events still come out first.
    void Close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        cv_.notify_all();
        space_cv_.notify_all();
    }

    // Drops every queued event, leaving open/closed state untouched, and wakes
    // blocked producers. For teardown paths that must not spend unbounded time
    // draining work nobody can consume anymore. Returns how many were dropped.
    std::size_t DiscardPending() {
        std::size_t dropped = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            dropped = events_.size();
            events_.clear();
        }
        space_cv_.notify_all();
        return dropped;
    }

    bool closed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return events_.size();
    }

    std::size_t max_size() const { return max_size_; }

private:
    // Caller holds mutex_. Every parked producer waits on the same predicate
    // (room for one more), so one freed slot needs one wake-up.
    std::optional<T> TakeFrontLocked() {
        if (events_.empty()) return std::nullopt;
        T event = std::move(events_.front());
        events_.pop_front();
        space_cv_.notify_one();
        return event;
    }

    mutable std::mutex mutex_;
    std::condition_variable cv_;        // consumer waits here for events
    std::condition_variable space_cv_;  // producers wait here for room
    std::deque<T> events_;
    std::size_t max_size_;
    bool closed_ = false;
};

}  // namespace minimal::common
