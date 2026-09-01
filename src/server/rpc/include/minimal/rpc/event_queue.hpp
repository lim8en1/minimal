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
// TryPush never blocks and is for the READER, which must not. Push's one
// non-blocking exit is drop-mode below: a cancel must be able to unwedge a
// worker parked on a full queue, and the event it was trying to deliver at
// that moment is the correct thing to lose.

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <utility>

namespace minimal::rpc {

template <typename T>
class EventQueue {
public:
    // Per-event byte cost. Null (the default) disables byte accounting -- the
    // tests drive the ordering/concurrency properties on trivial types. The
    // session's queue passes the protobuf cost: a COUNT-ONLY bound does not
    // bound RAM when one event can carry megabytes (an 8 KiB output chunk per
    // ToolOutputDelta at the 65,536-event cap is over half a gigabyte per
    // stalled client, and the queue exists precisely for the stalled client).
    using SizeOf = std::function<std::size_t(const T&)>;

    // max_bytes == 0 means no byte bound. The default 64 MiB is per session:
    // ~16x a legitimate 1M-token text response in flight, and the count cap
    // still applies beside it.
    static constexpr std::size_t kDefaultMaxBytes = 64ull << 20;

    explicit EventQueue(std::size_t max_size = kDefaultMaxSize,
                        std::size_t max_bytes = 0, SizeOf size_of = {})
        : max_size_(max_size), max_bytes_(max_bytes), size_of_(std::move(size_of)) {}

    // WORKER-THREAD push. Blocks while the queue is full (by count OR by
    // bytes), which is safe only because the worker is the thread permitted to
    // block -- never call this from the gRPC reader. Accepted after Close()
    // only to be discarded: a producer racing teardown should not have to
    // check first, and silently dropping is correct once there is provably no
    // one left to deliver to.
    //
    // Returns whether the event was queued. False means the queue is closed,
    // or drop-mode (below) latched and the queue is over its bounds -- the
    // caller decides what a refusal means. The stream sink's refusal is the
    // loop abort; sinks without a bool contract simply lose the event.
    bool Push(T event) {
        bool accepted = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            // Closed wakes it too, or a worker would park forever on a queue
            // whose consumer has already gone. Drop-mode wakes it to DROP,
            // not to block again -- that is the whole point of Cancel's
            // unwedge.
            space_cv_.wait(lock, [this, &event] {
                return closed_ || drops_ || Fits(event);
            });
            if (closed_) return false;
            if (!Fits(event)) return false;  // drop-mode latched: see below
            Accept(std::move(event));
            accepted = true;
        }
        cv_.notify_one();
        return accepted;
    }

    // The TurnCompleted-shaped push: drop-mode never applies. A cancel that
    // unwedges a worker on a full queue must not also swallow the turn's exit
    // signal -- a headless client detects the end of a turn by TurnCompleted
    // alone, and dropping it converts the unwedge back into a silent hang.
    // Still blocks for ROOM (the ordinary backpressure a full queue exerts,
    // released by the next drain or by Close at teardown); it only opts out
    // of the drop-mode refusal.
    bool PushTerminal(T event) {
        bool accepted = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
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
        // A successful drain ends drop-mode: room exists again, so ordinary
        // backpressure -- block, never drop -- is the right policy from here.
        drops_ = false;
        space_cv_.notify_one();
        return event;
    }

    // Non-blocking drain, for a consumer that also has other work to poll.
    std::optional<T> TryPop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (events_.empty()) return std::nullopt;
        T event = std::move(events_.front().event);
        bytes_total_ -= events_.front().bytes;
        events_.pop_front();
        drops_ = false;  // same self-heal as Pop
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

    // Latches drop-mode: until the consumer next drains (Pop/TryPop), Push
    // REFUSES over-bounds events instead of blocking on them. Cancel's
    // unwedge: a worker parked on a full queue -- a stalled-but-connected
    // client, the one state backpressure exists for -- was exactly the state
    // a cancel could not reach: the parked push blocked the very turn the
    // user asked to stop, and only stream close helped.
    //
    // Latched rather than one-shot so a producer parking a moment AFTER the
    // cancel gets the same way out, and so the self-heal is visible: the
    // latch clears at the next successful drain, when ordinary backpressure
    // resumes. NOT Close(): cancel ends a turn, not the session -- the queued
    // backlog stays deliverable, the queue stays open, and PushTerminal
    // events still wait for room.
    void AbortWaitingProducers() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) return;
            drops_ = true;
        }
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
    // Drop-mode, latched by AbortWaitingProducers, cleared by the next
    // successful drain: while set, Push refuses over-bounds events instead of
    // blocking the worker on them.
    bool drops_ = false;
};

}  // namespace minimal::rpc
