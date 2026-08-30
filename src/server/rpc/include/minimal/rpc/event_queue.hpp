#pragma once

// The session's outbound queue: many producers, one draining consumer.
//
// TWO INDEPENDENT REASONS this exists, and both are load-bearing:
//
// 1. gRPC permits one concurrent reader and one concurrent writer on a
//    ServerReaderWriter, but NOT concurrent Write() calls. Three sources produce
//    outbound events (the agent loop, subagent completions, read-only commands
//    running alongside a turn), so a single writer draining one queue is
//    required. A mutex at each Write() call site would be slower and easier to
//    get wrong.
//
// 2. THE QUEUE BELONGS TO THE SESSION, NOT THE STREAM. Events produced before a
//    client attaches buffer instead of dropping -- which is what lets a
//    subagent's loop start producing before its tab attaches, and means the
//    worker never has to know whether anyone is currently listening.
//
// See ARCHITECTURE.md "Concurrency model".
//
// Templated because none of the above depends on the payload type: keeping it
// generic lets the ordering and concurrency properties be tested against a
// trivial type, where a failure points at the queue rather than at protobuf.
//
// BOUNDED, and the bound has to exist because of reason 2. Buffering before
// attach is deliberate, but the initial-attach guard bounds TIME, not VOLUME: a
// local model streaming token-granularity deltas into a session nobody has
// attached to yet accumulates protobufs for the whole guard period, and a slow
// client has no time bound at all. Backpressure rather than dropping, because
// the events are a conversation and a hole in one is worse than a stall.
//
// The two push flavours match the thread contract exactly (see session.hpp):
// Push blocks when full and is for the WORKER, which is allowed to block;
// TryPush never blocks and is for the READER, which must not.

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

namespace minimal::rpc {

template <typename T>
class EventQueue {
public:
    explicit EventQueue(std::size_t max_size = kDefaultMaxSize) : max_size_(max_size) {}

    // WORKER-THREAD push. Blocks while the queue is full, which is safe only
    // because the worker is the thread permitted to block -- never call this
    // from the gRPC reader. Accepted after Close() only to be discarded: a
    // producer racing teardown should not have to check first, and silently
    // dropping is correct once there is provably no one left to deliver to.
    void Push(T event) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            // Closed wakes it too, or a worker would park forever on a queue
            // whose consumer has already gone.
            space_cv_.wait(lock, [this] { return closed_ || events_.size() < max_size_; });
            if (closed_) return;
            events_.push_back(std::move(event));
        }
        cv_.notify_one();
    }

    // READER-THREAD push. Never blocks; false means the queue is full and the
    // caller must decide what to do rather than stalling the read loop -- which
    // would be the deadlock the three-thread topology exists to prevent.
    bool TryPush(T event) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) return true;  // dropped on purpose, as above
            if (events_.size() >= max_size_) return false;
            events_.push_back(std::move(event));
        }
        cv_.notify_one();
        return true;
    }

    // The single consumer. Blocks until an event is available or the queue is
    // closed AND drained. nullopt means "no more events, ever".
    //
    // Draining before honouring the close is deliberate: Close() runs at session
    // teardown, and the events already queued are the ones explaining why the
    // session is ending. Discarding them would lose exactly the output worth
    // keeping.
    std::optional<T> Pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return !events_.empty() || closed_; });
        if (events_.empty()) return std::nullopt;
        T event = std::move(events_.front());
        events_.pop_front();
        space_cv_.notify_one();
        return event;
    }

    // Non-blocking drain, for a consumer that also has other work to poll.
    std::optional<T> TryPop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (events_.empty()) return std::nullopt;
        T event = std::move(events_.front());
        events_.pop_front();
        space_cv_.notify_one();
        return event;
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
    // Generous on purpose: this is a ceiling that catches a runaway producer,
    // not a tuning knob. A normal turn queues a few thousand deltas at most, so
    // a session only reaches this if nobody is draining -- which is the case
    // worth bounding.
    static constexpr std::size_t kDefaultMaxSize = 65536;

    mutable std::mutex mutex_;
    std::condition_variable cv_;        // consumer waits here for events
    std::condition_variable space_cv_;  // producers wait here for room
    std::deque<T> events_;
    std::size_t max_size_;
    bool closed_ = false;
};

}  // namespace minimal::rpc
