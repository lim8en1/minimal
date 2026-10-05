// Unit tier for the EventBroadcaster (S5.1) -- no gRPC, no Session, real
// protobuf events so the byte accounting is the production one. The properties
// under test are the ones the fan-out design stands on:
//
//   * broadcast order is identical across subscribers, and Publish never
//     blocks or refuses for capacity -- a full subscriber drops its OWN oldest
//     and latches a resync notice, and the producer's progress is unaffected;
//   * the zero-subscriber backlog buffers and seeds the FIRST subscriber (the
//     pre-attach semantics subagent attach relies on), while a LATER
//     subscriber is told to resync instead of silently missing events;
//   * drain-then-close: a queue closed by Unsubscribe/CloseAll delivers what
//     it holds before Pop reports exhaustion.

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "minimal/rpc/event_broadcaster.hpp"

using namespace minimal;
using minimal::rpc::EventBroadcaster;

namespace {

EventBroadcaster::Handle Subscribe(EventBroadcaster& broadcaster) {
    return broadcaster.Subscribe();
}

v1::ServerEvent Text(const std::string& session_id, const std::string& text) {
    v1::ServerEvent event;
    event.set_session_id(session_id);
    event.mutable_stream_event()->mutable_text_delta()->set_text(text);
    return event;
}

}  // namespace

TEST_CASE("publish broadcasts the same sequence to every subscriber", "[broadcaster]") {
    EventBroadcaster broadcaster{"s1"};
    auto a = Subscribe(broadcaster);
    auto b = Subscribe(broadcaster);
    // b attached LATE: it receives the one-time resync notice before anything
    // else. Drain it so the sequences below compare content only.
    const auto notice = broadcaster.TryPop(b);
    REQUIRE(notice.has_value());
    REQUIRE(notice->has_resync_required());

    for (int i = 0; i < 5; ++i) {
        REQUIRE(broadcaster.Publish(Text("s1", "e" + std::to_string(i))));
    }

    for (auto* sub : {&a, &b}) {
        for (int i = 0; i < 5; ++i) {
            const auto event = broadcaster.TryPop(*sub);
            REQUIRE(event.has_value());
            CHECK(event->stream_event().text_delta().text() == "e" + std::to_string(i));
        }
        CHECK(broadcaster.TryPop(*sub) == std::nullopt);
    }
}

TEST_CASE("a full subscriber drops its oldest and latches exactly one resync",
          "[broadcaster][invariant]") {
    // THE INVARIANT: Publish never blocks and never refuses for capacity --
    // the session's progress is never gated on any subscriber. The queue at
    // fault pays in its own oldest events, and one resync notice tells the
    // client to refetch, however many drops it took.
    EventBroadcaster broadcaster{"s1", /*max_events=*/8, /*max_bytes=*/0};
    auto slow = Subscribe(broadcaster);
    auto fast = Subscribe(broadcaster);
    {   // drain fast's late-attach resync notice
        const auto notice = broadcaster.TryPop(fast);
        REQUIRE(notice.has_value());
        REQUIRE(notice->has_resync_required());
    }

    // The fast subscriber keeps up with the producer -- a live writer,
    // interleaved here deterministically. The slow one never drains.
    std::vector<std::string> fast_saw;
    for (int i = 0; i < 100; ++i) {
        // Publish never blocks and never refuses for capacity, whatever the
        // slow subscriber is doing.
        REQUIRE(broadcaster.Publish(Text("s1", "e" + std::to_string(i))));
        while (const auto event = broadcaster.TryPop(fast)) {
            fast_saw.push_back(event->stream_event().text_delta().text());
        }
    }

    // The drainer lost nothing, in order.
    REQUIRE(fast_saw.size() == 100);
    for (int i = 0; i < 100; ++i) {
        CHECK(fast_saw[static_cast<std::size_t>(i)] == "e" + std::to_string(i));
    }

    // The slow subscriber holds the newest slice the cap allowed. The drop
    // latched at its first overflow; the notice rode the tail through the
    // later evictions (drop-oldest removes the head, so the tail-delivered
    // notice can never be silently evicted) and arrives after the events it
    // vouches for may have holes in.
    std::vector<std::string> slow_saw;
    std::optional<v1::ServerEvent> popped;
    while ((popped = broadcaster.TryPop(slow)).has_value()) {
        if (popped->has_resync_required()) {
            CHECK(popped->resync_required().reason() == "overflow");
        } else {
            slow_saw.push_back(popped->stream_event().text_delta().text());
        }
    }
    REQUIRE_FALSE(slow_saw.empty());
    CHECK(slow_saw.front() != "e0");   // the oldest were dropped
    CHECK(slow_saw.back() == "e99");   // the cap held: the newest slice is whole

    // A re-latch (another drop) does not queue a second notice.
    for (int i = 0; i < 20; ++i) REQUIRE(broadcaster.Publish(Text("s1", "more")));
    bool saw_second_resync = false;
    while (const auto event = broadcaster.TryPop(slow)) {
        if (event->has_resync_required()) saw_second_resync = true;
    }
    CHECK_FALSE(saw_second_resync);
}

TEST_CASE("the backlog seeds the first subscriber and is a resync for later ones",
          "[broadcaster]") {
    EventBroadcaster broadcaster{"s1"};
    // Produced while nobody is attached -- the subagent pre-attach window.
    REQUIRE(broadcaster.Publish(Text("s1", "pre-1")));
    REQUIRE(broadcaster.Publish(Text("s1", "pre-2")));
    CHECK(broadcaster.backlog_size() == 2);

    auto first = Subscribe(broadcaster);
    CHECK(broadcaster.backlog_size() == 0);   // consumed, not copied
    const auto a = broadcaster.TryPop(first);
    REQUIRE(a.has_value());
    CHECK(a->stream_event().text_delta().text() == "pre-1");
    const auto b = broadcaster.TryPop(first);
    REQUIRE(b.has_value());
    CHECK(b->stream_event().text_delta().text() == "pre-2");

    // A second subscriber missed everything the first took. The first one
    // detaches; "live" is produced into the zero-subscriber window and lands
    // in the backlog.
    broadcaster.Unsubscribe(first);
    REQUIRE(broadcaster.Publish(Text("s1", "live")));
    auto second = Subscribe(broadcaster);
    const auto notice = broadcaster.TryPop(second);
    REQUIRE(notice.has_value());
    REQUIRE(notice->has_resync_required());
    CHECK(notice->resync_required().reason() == "late_attach");
    const auto live = broadcaster.TryPop(second);
    REQUIRE(live.has_value());
    CHECK(live->stream_event().text_delta().text() == "live");
}

TEST_CASE("a latched backlog resyncs the first subscriber instead of a partial replay",
          "[broadcaster]") {
    EventBroadcaster broadcaster{"s1", /*max_events=*/4, /*max_bytes=*/0};
    for (int i = 0; i < 10; ++i) REQUIRE(broadcaster.Publish(Text("s1", "e")));
    CHECK(broadcaster.backlog_size() == 4);

    auto sub = Subscribe(broadcaster);
    const auto first = broadcaster.TryPop(sub);
    REQUIRE(first.has_value());
    REQUIRE(first->has_resync_required());
    CHECK(first->resync_required().reason() == "overflow");
    // The partial backlog still rides along -- labelled incomplete by the
    // notice that precedes it.
    std::size_t seeded = 0;
    while (broadcaster.TryPop(sub).has_value()) ++seeded;
    CHECK(seeded == 4);
}

TEST_CASE("the backlog has its own, smaller byte bound than a live subscriber",
          "[broadcaster]") {
    // A detached session's backlog is resident memory nobody is reading, so
    // it is bounded independently of (and far below) a subscriber queue. A
    // drop latches it exactly like the count bound does.
    const std::string payload(1024, 'x');
    const std::size_t one = Text("s1", payload).ByteSizeLong();
    EventBroadcaster broadcaster{"s1", /*max_events=*/1000, /*max_bytes=*/0,
                                 /*max_backlog_bytes=*/one * 3};
    for (int i = 0; i < 10; ++i) {
        REQUIRE(broadcaster.Publish(Text("s1", payload + std::to_string(i % 10))));
    }
    CHECK(broadcaster.backlog_size() <= 3);

    auto sub = Subscribe(broadcaster);
    const auto first = broadcaster.TryPop(sub);
    REQUIRE(first.has_value());
    REQUIRE(first->has_resync_required());
    CHECK(first->resync_required().reason() == "overflow");
    std::optional<v1::ServerEvent> last;
    while (auto event = broadcaster.TryPop(sub)) last = std::move(event);
    REQUIRE(last.has_value());
    CHECK(last->stream_event().text_delta().text() == payload + "9");  // newest kept

    // The live subscriber is NOT held to the backlog's bound (max_bytes=0).
    for (int i = 0; i < 10; ++i) REQUIRE(broadcaster.Publish(Text("s1", payload)));
    CHECK(broadcaster.queued(sub) == 10);
}

TEST_CASE("PublishToIfAbsent dedups only against events of the same kind", "[broadcaster]") {
    // The duplicate check filters on the payload case before serializing; a
    // prompt queued BEHIND many other-kind events is still found, and a
    // different-kind event never matches.
    EventBroadcaster broadcaster{"s1"};
    auto sub = Subscribe(broadcaster);
    for (int i = 0; i < 100; ++i) REQUIRE(broadcaster.Publish(Text("s1", "noise")));

    v1::ServerEvent prompt;
    prompt.set_session_id("s1");
    prompt.mutable_resync_required()->set_reason("stand-in for a prompt");
    CHECK(broadcaster.PublishToIfAbsent(sub, prompt));
    CHECK(broadcaster.PublishToIfAbsent(sub, prompt));  // already queued
    CHECK(broadcaster.queued(sub) == 101);
}

TEST_CASE("PublishToIfAbsent skips an event the subscriber already holds", "[broadcaster]") {
    EventBroadcaster broadcaster{"s1"};
    auto sub = Subscribe(broadcaster);

    CHECK(broadcaster.PublishToIfAbsent(sub, Text("s1", "prompt")));
    CHECK(broadcaster.PublishToIfAbsent(sub, Text("s1", "prompt")));  // identical: not queued twice
    CHECK(broadcaster.PublishToIfAbsent(sub, Text("s1", "other")));

    std::size_t queued = 0;
    while (broadcaster.TryPop(sub).has_value()) ++queued;
    CHECK(queued == 2);
}

TEST_CASE("unsubscribed and closed broadcasters refuse and wake their writers",
          "[broadcaster]") {
    EventBroadcaster broadcaster{"s1"};
    auto a = Subscribe(broadcaster);
    auto b = Subscribe(broadcaster);
    (void)broadcaster.TryPop(b);   // b's late-attach resync notice

    broadcaster.Unsubscribe(a);
    CHECK(broadcaster.PublishTo(a, Text("s1", "x")) == false);

    REQUIRE(broadcaster.Publish(Text("s1", "y")));
    REQUIRE(broadcaster.TryPop(b).has_value());

    broadcaster.CloseAll();
    CHECK(broadcaster.closed());
    CHECK(broadcaster.Publish(Text("s1", "z")) == false);
    CHECK(broadcaster.subscriber_count() > 0);   // handles remain until Unsubscribe
    // Drain-then-close: what was queued still comes out, then exhaustion.
    while (broadcaster.TryPop(b).has_value()) {
    }

    // Subscribe after close: a closed-and-empty queue, so a late writer exits.
    auto late = Subscribe(broadcaster);
    CHECK(broadcaster.TryPop(late) == std::nullopt);
}

TEST_CASE("Pop blocks until an event or a close, then drains before exhausting",
          "[broadcaster]") {
    EventBroadcaster broadcaster{"s1"};
    auto sub = Subscribe(broadcaster);

    std::optional<v1::ServerEvent> popped;
    std::atomic<bool> done{false};
    std::thread reader([&] {
        popped = broadcaster.Pop(sub);
        done.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK_FALSE(done.load(std::memory_order_acquire));   // parked, not spinning

    REQUIRE(broadcaster.Publish(Text("s1", "wake")));
    reader.join();
    REQUIRE(popped.has_value());
    CHECK(popped->stream_event().text_delta().text() == "wake");

    // Close wakes a second park; queued events still come out first.
    REQUIRE(broadcaster.Publish(Text("s1", "last")));
    broadcaster.CloseAll();
    const auto final_event = broadcaster.Pop(sub);
    REQUIRE(final_event.has_value());
    CHECK(final_event->stream_event().text_delta().text() == "last");
    CHECK(broadcaster.Pop(sub) == std::nullopt);
}

