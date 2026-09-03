// Unit tier for EventTranslator -- the seam agent::SessionEvent crosses to
// become a v1::ServerEvent on a real TransportChannels. No Session involved.

#include <catch2/catch_test_macros.hpp>

#include "minimal/rpc/event_translator.hpp"

using namespace minimal;
using rpc::EventTranslator;
using rpc::MakeTransportChannels;

TEST_CASE("EventTranslator publishes a translated SessionEvent to the broadcaster",
          "[event_translator]") {
    auto transport = MakeTransportChannels("sess-1");
    EventTranslator translator{"sess-1", transport};

    auto subscriber = transport->queue.Subscribe();
    REQUIRE(translator.Publish(agent::SessionEnded{}));

    const auto out = transport->queue.TryPop(subscriber);
    REQUIRE(out.has_value());
    CHECK(out->session_id() == "sess-1");
    CHECK(out->has_session_ended());
}

TEST_CASE("EventTranslator is usable directly as a SessionEventSink", "[event_translator]") {
    auto transport = MakeTransportChannels("sess-2");
    EventTranslator translator{"sess-2", transport};
    agent::SessionEventSink sink = translator;

    auto subscriber = transport->queue.Subscribe();
    CHECK(sink(agent::SessionError{"boom"}));

    const auto out = transport->queue.TryPop(subscriber);
    REQUIRE(out.has_value());
    CHECK(out->error().message() == "boom");
}

TEST_CASE("EventTranslator copies independently keep the transport alive", "[event_translator]") {
    // Mirrors the shared_ptr-by-value capture pattern a detached background
    // watcher relies on: a copy of the translator must go on publishing after
    // the original (and its owner) is gone.
    auto transport = MakeTransportChannels("sess-3");
    auto subscriber = transport->queue.Subscribe();

    agent::SessionEventSink sink;
    {
        EventTranslator translator{"sess-3", transport};
        sink = translator;
    }
    CHECK(sink(agent::SessionEnded{}));
    CHECK(transport->queue.TryPop(subscriber).has_value());
}

TEST_CASE("EventTranslator reports false once the broadcaster is closed", "[event_translator]") {
    auto transport = MakeTransportChannels("sess-4");
    EventTranslator translator{"sess-4", transport};
    transport->queue.CloseAll();
    CHECK_FALSE(translator.Publish(agent::SessionEnded{}));
}
