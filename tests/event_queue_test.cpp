#include <chrono>
#include <map>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "deadline.hpp"
#include "minimal/rpc/event_queue.hpp"

using minimal::rpc::EventQueue;
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
