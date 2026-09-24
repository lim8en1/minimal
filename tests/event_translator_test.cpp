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

TEST_CASE("EventTranslator publishes through the SessionTransport interface",
          "[event_translator]") {
    auto transport = MakeTransportChannels("sess-2");
    auto translator = std::make_shared<EventTranslator>("sess-2", transport);
    std::shared_ptr<agent::SessionTransport> session_transport = translator;

    auto subscriber = transport->queue.Subscribe();
    CHECK(session_transport->Publish(agent::SessionError{"boom"}));

    const auto out = transport->queue.TryPop(subscriber);
    REQUIRE(out.has_value());
    CHECK(out->error().message() == "boom");
}

TEST_CASE("a sink capturing the transport keeps it alive past its owner", "[event_translator]") {
    // Mirrors the shared_ptr-by-value capture Session's sinks use: a detached
    // background watcher must go on publishing after the Session is gone.
    auto transport = MakeTransportChannels("sess-3");
    auto subscriber = transport->queue.Subscribe();

    agent::SessionEventSink sink;
    {
        std::shared_ptr<agent::SessionTransport> owner =
            std::make_shared<EventTranslator>("sess-3", transport);
        sink = [owner](const agent::SessionEvent& event) { return owner->Publish(event); };
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
