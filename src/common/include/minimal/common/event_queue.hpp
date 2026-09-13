#pragma once

// A bounded, thread-safe FIFO: many producers, one draining consumer.
//
// Originally built as the session's outbound gRPC queue -- one thread may
// concurrently Write() a ServerReaderWriter, so the agent loop, subagent
// completions and read-only commands running alongside a turn all had to
// funnel through one draining writer -- but EventBroadcaster (see
// src/server/rpc/include/minimal/rpc/event_broadcaster.hpp) took that role
// over with a never-blocking drop-oldest design. The terminal-push and
// drop-mode machinery that supported the old queue's blocking producers
// (PushTerminal, AbortWaitingProducers) had no production caller left and has
// been DELETED; history has it. Today's production users are two task queues,
// `EventQueue<std::function<void()>>` in HookDispatcher and
// SessionObserverRegistry, that use Push/TryPush/Pop/Close/DiscardPending on
// count-bounded (no SizeOf) queues; the byte-accounting path is exercised by
// tests/event_queue_test.cpp only. Kept in common/ because the
// ordering/backpressure invariants are exactly what a "many producers queue
// work behind one draining consumer" primitive needs, whichever caller
// reaches for it next.
//
// Templated because none of the above depends on the payload type: keeping it
// generic lets the ordering and concurrency properties be tested against a
// trivial type, where a failure points at the queue rather than at protobuf.
//
// BOUNDED: buffering is deliberate (a producer that starts before anyone is
// draining should not have to know that, and should not lose events for it),
// but unbounded buffering behind a stalled consumer is a leak wearing a
// feature's clothes. Backpressure rather than dropping, because a hole in the
// middle of ordered work is worse than the producer stalling.
//
// The two push flavours match a WORKER/READER thread split: Push blocks when
// full and is for the thread allowed to block; TryPush never blocks and is for
// a thread that must not (draining alongside other work).

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace minimal::common {

template <typename T>
class EventQueue {
public:
    // Per-event byte cost. Null (the default) disables byte accounting -- a
    // COUNT-ONLY bound does not bound RAM when one event can carry megabytes
    // (an 8 KiB output chunk per delta at a 65,536-event cap is over half a
    // gigabyte per stalled consumer, and the point of this queue is to survive
    // a stalled consumer). A caller that wants a byte bound MUST supply this;
    // see the constructor below for what happens if it doesn't.
    using SizeOf = std::function<std::size_t(const T&)>;

    // A reasonable byte bound for callers that do enable byte accounting: ~16x
    // a legitimate 1M-token text response in flight, with the count cap still
    // applying beside it. NOT the default max_bytes (that stays 0, i.e. no
    // byte bound) -- callers opt in explicitly by passing both this and a
    // SizeOf.
    static constexpr std::size_t kDefaultMaxBytes = 64ull << 20;

    // max_bytes == 0 means no byte bound. Passing a nonzero max_bytes without a
    // size_of is refused rather than silently accepted: every event would cost
    // 0 by the null SizeOf's convention (see Fits/Accept below), so the byte
    // cap would never trip and buffered_bytes() would always read 0 -- a
    // caller who thinks they set a byte bound and, in production, do not have
    // one is exactly the failure this guard exists to catch at construction
    // instead of silently at runtime.
    explicit EventQueue(std::size_t max_size = kDefaultMaxSize,
                        std::size_t max_bytes = 0, SizeOf size_of = {})
        : max_size_(max_size), max_bytes_(max_bytes), size_of_(std::move(size_of)) {
        if (max_bytes_ != 0 && !size_of_) {
            throw std::invalid_argument(
                "EventQueue: max_bytes > 0 requires a SizeOf function; without one every "
                "event costs 0 bytes and the byte cap never trips");
        }
    }

    // WORKER-THREAD push. Blocks while the queue is full (by count OR by
    // bytes), which is safe only because the worker is the thread permitted to
    // block -- never call this from a thread that must not stall. Refused
    // rather than accepted after Close(): a producer racing teardown gets the
    // same false TryPush already returns, so callers do not need two
    // "did this actually happen" idioms depending on which push they used.
    //
    // Returns whether the event was queued. False means the queue is closed;
    // the caller decides what a refusal means.
    bool Push(T event) {
        bool accepted = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            // Closed wakes it too, or a worker would park forever on a queue
            // whose consumer has already gone.
            space_cv_.wait(lock, [this, &event] { return closed_ || Fits(event); });
            if (closed_) return false;
            Accept(std::move(event));
            accepted = true;
        }
        cv_.notify_one();
        return accepted;
    }

    // READER-THREAD push. Never blocks; false means the queue refused the
    // event -- full by count or bytes, or closed -- and the caller must decide
    // what to do rather than stalling the read loop, which would be the
    // deadlock the three-thread topology exists to prevent. (It used to return
    // TRUE on a closed queue as "dropped on purpose": the refusal paths'
    // drop-logging then never fired after Terminate, and the logging contract
    // the call sites promise was false on exactly the shutdown path.)
    bool TryPush(T event) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) return false;
            if (!Fits(event)) return false;
            Accept(std::move(event));
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
        T event = std::move(events_.front().event);
        bytes_total_ -= events_.front().bytes;
        events_.pop_front();
        // notify_all, not notify_one: waiters' Fits(event) predicates differ by
        // the size of THEIR OWN pending event, so one drain can free room for a
        // small waiter while notify_one happens to wake a large one, which
        // re-waits without re-notifying -- the small waiter starves until the
        // next drain. All woken waiters re-check their own predicate and
        // re-park if it is still false, so this costs some wasted wakeups, not
        // correctness.
        space_cv_.notify_all();
        return event;
    }

    // Non-blocking drain, for a consumer that also has other work to poll.
    std::optional<T> TryPop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (events_.empty()) return std::nullopt;
        T event = std::move(events_.front().event);
        bytes_total_ -= events_.front().bytes;
        events_.pop_front();
        space_cv_.notify_all();  // same non-uniform-predicate reasoning as Pop
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

    // Drops every queued event; open/closed state are untouched, and this is a
    // drain like Pop/TryPop, not a reset -- it clears bytes_total_ (an empty
    // deque has zero bytes buffered, and leaving the stale total in place would
    // make Fits() refuse everything but a single event forever). For teardown
    // paths that must not spend unbounded time draining work whose results
    // nobody can consume anymore (the hook dispatcher's destructor: N queued
    // hooks x up to a 60 s uncancellable floor each). Blocked producers wake
    // with the queue roomy.
    void DiscardPending() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            events_.clear();
            bytes_total_ = 0;
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

    // Bytes currently buffered; 0 forever when no size function was provided.
    std::size_t buffered_bytes() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return bytes_total_;
    }

private:
    // Generous on purpose: this is a ceiling that catches a runaway producer,
    // not a tuning knob. A normal turn queues a few thousand deltas at most, so
    // a session only reaches this if nobody is draining -- which is the case
    // worth bounding.
    static constexpr std::size_t kDefaultMaxSize = 65536;

    // Would accepting `event` leave the queue within both bounds? A single
    // event LARGER than the byte cap is accepted onto an EMPTY queue -- a
    // producer that could never fit its one event would block forever, and
    // delivering one oversized event is strictly better than a deadlock.
    bool Fits(const T& event) const {
        if (events_.size() >= max_size_) return false;
        if (max_bytes_ == 0) return true;
        const std::size_t bytes = size_of_ ? size_of_(event) : 0;
        return events_.empty() || bytes_total_ + bytes <= max_bytes_;
    }

    void Accept(T&& event) {
        std::size_t bytes = 0;
        if (size_of_) {
            bytes = size_of_(event);
            bytes_total_ += bytes;
        }
        events_.push_back(Item{std::move(event), bytes});
    }

    struct Item {
        T event;
        std::size_t bytes = 0;
    };

    mutable std::mutex mutex_;
    std::condition_variable cv_;        // consumer waits here for events
    std::condition_variable space_cv_;  // producers wait here for room
    std::deque<Item> events_;
    std::size_t max_size_;
    std::size_t max_bytes_ = 0;
    std::size_t bytes_total_ = 0;
    SizeOf size_of_;
    bool closed_ = false;
};

}  // namespace minimal::common
