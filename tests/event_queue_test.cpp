#include <chrono>
#include <map>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "deadline.hpp"
#include "minimal/common/event_queue.hpp"

using minimal::common::EventQueue;
using minimal::testing::RunInBackground;
using namespace std::chrono_literals;

namespace {

// Consumers capture the queue by shared_ptr, never by reference: if a blocking
// invariant breaks, the consumer thread wedges and is abandoned, and it must not
// be left pointing at a destroyed queue. See tests/support/deadline.hpp.
template <typename T>
auto PopInBackground(std::shared_ptr<EventQueue<T>> queue) {
    return RunInBackground([queue] { return queue->Pop(); });
}

}  // namespace

TEST_CASE("events queued before a consumer attaches are delivered in order", "[event_queue]") {
    // The property that makes the queue session-owned rather than stream-owned:
    // a subagent's loop produces events before its tab attaches, and the worker
    // must never have to know whether anyone is listening yet.
    EventQueue<int> queue;
    for (int i = 0; i < 5; ++i) queue.Push(i);

    CHECK(queue.size() == 5);
    for (int i = 0; i < 5; ++i) {
        const auto event = queue.Pop();
        REQUIRE(event.has_value());
        CHECK(*event == i);
    }
}

TEST_CASE("a consumer blocks until an event arrives", "[event_queue]") {
    auto queue = std::make_shared<EventQueue<int>>();
    auto consumer = PopInBackground(queue);
    CHECK_FALSE(consumer.WaitFor(100ms));

    queue->Push(42);
    REQUIRE(consumer.Ready());
    CHECK(consumer.Get().value() == 42);
}

TEST_CASE("closing wakes a blocked consumer", "[event_queue]") {
    // Session teardown closes the queue; if this hangs, the writer thread never
    // joins and terminating a session leaks it.
    auto queue = std::make_shared<EventQueue<int>>();
    auto consumer = PopInBackground(queue);
    CHECK_FALSE(consumer.WaitFor(50ms));

    queue->Close();
    REQUIRE(consumer.Ready());
    CHECK_FALSE(consumer.Get().has_value());
}

TEST_CASE("a closed queue drains its backlog before reporting closed", "[event_queue]") {
    // Close() runs at teardown, and the events already queued are the ones that
    // explain why the session is ending. Discarding them would lose exactly the
    // output worth keeping.
    EventQueue<int> queue;
    queue.Push(1);
    queue.Push(2);
    queue.Close();

    CHECK(queue.Pop().value() == 1);
    CHECK(queue.Pop().value() == 2);
    CHECK_FALSE(queue.Pop().has_value());
}

TEST_CASE("pushing after close is discarded rather than buffered forever", "[event_queue]") {
    // A producer racing teardown should not have to check first; once there is
    // provably nobody left to deliver to, dropping is the correct outcome.
    EventQueue<int> queue;
    queue.Close();
    queue.Push(1);
    CHECK(queue.size() == 0);
    CHECK_FALSE(queue.Pop().has_value());
}

TEST_CASE("close is idempotent", "[event_queue]") {
    EventQueue<int> queue;
    queue.Close();
    queue.Close();
    CHECK(queue.closed());
    CHECK_FALSE(queue.Pop().has_value());
}

TEST_CASE("TryPop never blocks on an empty queue", "[event_queue]") {
    EventQueue<int> queue;
    CHECK_FALSE(queue.TryPop().has_value());
    queue.Push(7);
    CHECK(queue.TryPop().value() == 7);
}

TEST_CASE("concurrent producers lose no events and preserve per-producer order",
          "[event_queue]") {
    // The real topology: the agent loop, subagent completions, and read-only
    // commands all push while one writer drains. Global order across producers
    // is not defined and must not be asserted -- but nothing may be lost,
    // duplicated, or reordered WITHIN a producer, since that is what would
    // corrupt a token stream.
    constexpr int kProducers = 8;
    constexpr int kPerProducer = 500;

    auto queue = std::make_shared<EventQueue<std::pair<int, int>>>();
    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([queue, p] {
            for (int i = 0; i < kPerProducer; ++i) queue->Push({p, i});
        });
    }

    auto received = std::make_shared<std::map<int, std::vector<int>>>();
    auto consumer = RunInBackground([queue, received] {
        for (int n = 0; n < kProducers * kPerProducer; ++n) {
            const auto event = queue->Pop();
            if (!event) return false;
            (*received)[event->first].push_back(event->second);
        }
        return true;
    });

    REQUIRE(consumer.WaitFor(10s));
    for (auto& producer : producers) producer.join();
    REQUIRE(consumer.Get());

    REQUIRE(received->size() == kProducers);
    for (const auto& [producer, sequence] : *received) {
        INFO("producer " << producer);
        REQUIRE(sequence.size() == kPerProducer);
        for (int i = 0; i < kPerProducer; ++i) {
            REQUIRE(sequence[i] == i);  // no loss, no duplication, no reordering
        }
    }
}

TEST_CASE("a move-only payload is not copied through the queue", "[event_queue]") {
    // ServerEvent is a protobuf message; queuing must move it, not copy it, or
    // every streamed token pays for a deep copy.
    EventQueue<std::unique_ptr<int>> queue;
    queue.Push(std::make_unique<int>(5));
    const auto event = queue.Pop();
    REQUIRE(event.has_value());
    REQUIRE(*event != nullptr);
    CHECK(**event == 5);
}

namespace {

// A sized test type: the byte accounting needs a cost per event, the way the
// session queue passes the protobuf size.
struct Sized {
    std::string payload;
};

std::size_t PayloadBytes(const Sized& event) { return event.payload.size(); }

}  // namespace

TEST_CASE("the byte bound refuses events past the cap, on both push flavours",
          "[event_queue][invariant]") {
    // A count-only bound does not bound RAM when one event can carry
    // megabytes: bash streams 8 KiB output chunks, so the 65,536-event cap was
    // ~540 MB per stalled client -- and the queue exists precisely for the
    // stalled client. Removing the byte accounting fails this test.
    EventQueue<Sized> queue(16, /*max_bytes=*/100, PayloadBytes);

    CHECK(queue.TryPush(Sized{std::string(60, 'x')}));
    CHECK(queue.buffered_bytes() == 60);
    CHECK_FALSE(queue.TryPush(Sized{std::string(60, 'y')}));  // would exceed 100
    CHECK(queue.TryPush(Sized{std::string(40, 'z')}));        // exactly at the cap
    CHECK(queue.buffered_bytes() == 100);
    CHECK_FALSE(queue.TryPush(Sized{"a"}));

    // Pop frees bytes, not just slots.
    (void)queue.Pop();
    CHECK(queue.buffered_bytes() == 40);
    CHECK(queue.TryPush(Sized{std::string(60, 'w')}));
}

TEST_CASE("a lone event larger than the byte cap is delivered, never deadlocked",
          "[event_queue]") {
    // A producer whose ONE event can never fit must not block forever:
    // delivering one oversized event is strictly better than a deadlock. It is
    // accepted onto an empty queue and refused while anything is buffered.
    EventQueue<Sized> queue(16, /*max_bytes=*/10, PayloadBytes);
    CHECK(queue.TryPush(Sized{std::string(100, 'x')}));
    CHECK(queue.buffered_bytes() == 100);
    CHECK_FALSE(queue.TryPush(Sized{"tiny"}));
    (void)queue.Pop();
    CHECK(queue.TryPush(Sized{"tiny"}));
}

TEST_CASE("Push blocks on the byte bound and wakes on Pop and Close", "[event_queue]") {
    auto queue_ptr = std::make_shared<EventQueue<Sized>>(16, 100, PayloadBytes);
    queue_ptr->TryPush(Sized{std::string(90, 'x')});

    // A push that does not fit parks; Pop frees room and it lands.
    auto producer = minimal::testing::RunInBackground([queue_ptr] {
        queue_ptr->Push(Sized{std::string(90, 'y')});
        return true;
    });
    std::this_thread::sleep_for(50ms);
    CHECK_FALSE(producer.Ready());
    (void)queue_ptr->Pop();
    CHECK(producer.WaitFor(2s));
    CHECK(queue_ptr->buffered_bytes() == 90);

    // A push parked at the cap wakes on Close -- the Terminate ordering's
    // wake-up, now also reachable through the byte bound.
    auto stalled = minimal::testing::RunInBackground([queue_ptr] {
        queue_ptr->Push(Sized{std::string(90, 'z')});
        return true;
    });
    std::this_thread::sleep_for(50ms);
    queue_ptr->Close();
    CHECK(stalled.WaitFor(2s));
}

TEST_CASE("TryPush returns false on a closed queue, so refusal logging fires",
          "[event_queue][invariant]") {
    // It used to return TRUE on a closed queue as "dropped on purpose", which
    // made every refusal path's drop-logging dead exactly at Terminate -- the
    // logging contract the call sites promise was false on the shutdown path.
    EventQueue<int> queue;
    queue.Close();
    CHECK_FALSE(queue.TryPush(1));
}

TEST_CASE("AbortWaitingProducers wakes a parked Push, which drops its event",
          "[event_queue][invariant]") {
    // THE INVARIANT: Cancel's unwedge. A worker parked on a full queue -- a
    // stalled-but-connected client -- was exactly the state a cancel could
    // not reach: the parked push blocked the very turn the user asked to
    // stop. The abort must wake it to DROP, not leave it parked.
    auto queue = std::make_shared<EventQueue<Sized>>(16, 100, PayloadBytes);
    REQUIRE(queue->TryPush(Sized{std::string(90, 'x')}));
    auto producer = RunInBackground([queue] { return queue->Push(Sized{std::string(90, 'y')}); });
    std::this_thread::sleep_for(50ms);
    CHECK_FALSE(producer.Ready());

    queue->AbortWaitingProducers();
    REQUIRE(producer.WaitFor(2s));
    CHECK_FALSE(producer.Get());   // refused, and...
    CHECK(queue->size() == 1);     // ...dropped, not queued
}

TEST_CASE("drop-mode self-heals at the next drain", "[event_queue][invariant]") {
    // Latched rather than one-shot so a producer parking a moment AFTER the
    // abort gets the same way out -- but the latch must not be permanent, or
    // one cancel would degrade the queue for its whole life.
    EventQueue<int> queue(2);  // count cap 2
    queue.Push(1);
    queue.Push(2);
    queue.AbortWaitingProducers();
    // While latched, an over-bounds push refuses immediately instead of parking.
    CHECK_FALSE(queue.Push(3));
    CHECK(queue.size() == 2);
    // The drain clears the latch: the same push is accepted now.
    CHECK(queue.Pop().value() == 1);
    CHECK(queue.Push(3));
    // ...and ordinary backpressure is back: a push at the cap parks.
    auto producer = RunInBackground([&queue] { return queue.Push(4); });
    std::this_thread::sleep_for(50ms);
    CHECK_FALSE(producer.Ready());
    CHECK(queue.Pop().value() == 2);
    REQUIRE(producer.WaitFor(2s));
    CHECK(producer.Get());
    CHECK(queue.Pop().value() == 3);
    CHECK(queue.Pop().value() == 4);
}

TEST_CASE("PushTerminal opts out of drop-mode, not out of backpressure", "[event_queue]") {
    // A cancel that unwedges a worker on a full queue must not also swallow
    // the turn's exit signal: TurnCompleted is how a headless client detects
    // the end of a turn at all. It still waits for room like any other worker
    // push -- only the drop-mode refusal is opted out of.
    EventQueue<int> queue(2);
    queue.Push(1);
    queue.Push(2);
    queue.AbortWaitingProducers();
    CHECK_FALSE(queue.Push(3));

    auto terminal = RunInBackground([&queue] { return queue.PushTerminal(3); });
    std::this_thread::sleep_for(50ms);
    CHECK_FALSE(terminal.Ready());  // still blocked -- for room, not by drop-mode
    CHECK(queue.Pop().value() == 1);
    REQUIRE(terminal.WaitFor(2s));
    CHECK(terminal.Get());
}

TEST_CASE("DiscardPending resets the byte total, not just the event count",
          "[event_queue][invariant]") {
    // events_.clear() alone left bytes_total_ pointing at bytes that no longer
    // exist, and nothing could ever subtract them back out (Pop/TryPop
    // subtract per-event, and there are no events left to pop) -- Fits()'s
    // only escape for an over-bounds queue is an EMPTY deque, so the queue
    // degraded to holding exactly one event at a time for the rest of its
    // life.
    EventQueue<Sized> queue(16, /*max_bytes=*/100, PayloadBytes);
    REQUIRE(queue.TryPush(Sized{std::string(100, 'x')}));
    REQUIRE(queue.buffered_bytes() == 100);

    queue.DiscardPending();
    CHECK(queue.size() == 0);
    CHECK(queue.buffered_bytes() == 0);

    // Proof the degradation is gone: TWO events now fit, not just one.
    CHECK(queue.TryPush(Sized{std::string(40, 'y')}));
    CHECK(queue.TryPush(Sized{std::string(40, 'z')}));
    CHECK(queue.buffered_bytes() == 80);
}

TEST_CASE("DiscardPending clears the drop latch too", "[event_queue][invariant]") {
    // The self-heal (see "drop-mode self-heals at the next drain" above) lived
    // only in Pop/TryPop; DiscardPending is a drain that frees MAXIMUM room
    // (the whole queue empties) and must clear the latch for the same reason
    // -- otherwise a teardown that discards pending work (DiscardPending's own
    // documented use) leaves drops_ permanently latched, since no Pop is
    // coming afterward to clear it.
    EventQueue<int> queue(2);
    queue.Push(1);
    queue.Push(2);
    queue.AbortWaitingProducers();
    CHECK_FALSE(queue.Push(3));  // drop-mode latched, refuses immediately

    queue.DiscardPending();
    CHECK(queue.size() == 0);

    // Ordinary backpressure is back: a push now blocks for room instead of
    // being refused outright.
    queue.Push(4);
    queue.Push(5);
    auto producer = RunInBackground([&queue] { return queue.Push(6); });
    std::this_thread::sleep_for(50ms);
    CHECK_FALSE(producer.Ready());
    (void)queue.Pop();
    REQUIRE(producer.WaitFor(2s));
    CHECK(producer.Get());
}

TEST_CASE("a nonzero byte bound without a SizeOf is refused at construction",
          "[event_queue][invariant]") {
    // Without a SizeOf every event costs 0 by convention (see Fits/Accept), so
    // the byte cap would never trip and buffered_bytes() would always read 0
    // -- a caller who thinks they configured a byte bound and, in production,
    // do not have one. Caught here instead of silently at runtime.
    CHECK_THROWS_AS((EventQueue<int>(16, /*max_bytes=*/100)), std::invalid_argument);
    // max_bytes == 0 (the default) needs no SizeOf -- there is no bound to
    // silently fail to enforce.
    CHECK_NOTHROW(EventQueue<int>(16));
}

TEST_CASE("a drain wakes every parked producer, not just one", "[event_queue][invariant]") {
    // notify_one on space_cv_ could wake a waiter whose OWN event still does
    // not fit while a different waiter's would -- the woken one re-parks
    // without re-notifying, and the other starves until some later drain
    // happens to pick it. Two producers with different-sized pending events,
    // one Pop that frees room for only the smaller one: both must be woken
    // (and re-check their own predicate), so the small one is not left parked
    // with room actually available for it.
    //
    // Queue at the 100-byte cap (95 + 5): NEITHER producer's event fits yet.
    // Popping the 95-byte item leaves 5 bytes buffered, so the small
    // producer's 5-byte push now fits (5+5<=100) but the big producer's
    // 96-byte push still does not (5+96>100) -- exactly the asymmetric-room
    // case notify_one could get wrong.
    auto queue = std::make_shared<EventQueue<Sized>>(16, /*max_bytes=*/100, PayloadBytes);
    REQUIRE(queue->TryPush(Sized{std::string(95, 'a')}));
    REQUIRE(queue->TryPush(Sized{std::string(5, 'b')}));

    auto big = RunInBackground([queue] { return queue->Push(Sized{std::string(96, 'c')}); });
    auto small = RunInBackground([queue] { return queue->Push(Sized{std::string(5, 'd')}); });
    std::this_thread::sleep_for(50ms);
    CHECK_FALSE(big.Ready());
    CHECK_FALSE(small.Ready());

    // Frees exactly the 95-byte item: only `small`'s event can possibly fit now.
    const auto popped = queue->Pop();
    REQUIRE(popped->payload.size() == 95);
    CHECK(small.WaitFor(2s));
    CHECK(small.Get());
    CHECK_FALSE(big.Ready());  // still correctly parked; not starved, not wrongly woken-and-accepted

    // Clean up the still-parked big producer: its 96-byte event only fits an
    // EMPTY queue (Fits' oversized-event escape hatch), so drain everything.
    (void)queue->Pop();  // the original 5-byte item
    (void)queue->Pop();  // small's 5-byte item
    CHECK(big.WaitFor(2s));
    CHECK(big.Get());
}
