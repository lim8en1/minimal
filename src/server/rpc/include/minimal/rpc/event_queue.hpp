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
    // Any thread. Accepted after Close() only to be discarded -- a producer
    // racing teardown should not have to check first, and silently dropping is
    // correct once there is provably no one left to deliver to.
    void Push(T event) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) return;
            events_.push_back(std::move(event));
        }
        cv_.notify_one();
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
        return event;
    }

    // Non-blocking drain, for a consumer that also has other work to poll.
    std::optional<T> TryPop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (events_.empty()) return std::nullopt;
        T event = std::move(events_.front());
        events_.pop_front();
        return event;
    }

    // Idempotent. Wakes a blocked consumer; queued events still come out first.
    void Close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        cv_.notify_all();
    }

    bool closed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return events_.size();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<T> events_;
    bool closed_ = false;
};

}  // namespace minimal::rpc
