#include <chrono>
#include <memory>
#include <optional>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "deadline.hpp"
#include "minimal/rpc/wire_confirmation_broker.hpp"

using namespace minimal;
using minimal::testing::RunInBackground;
using namespace std::chrono_literals;

namespace {

// The three objects a confirmation round-trip needs -- mirrors
// wire_permission_checker_test.cpp's Harness, the shared skeleton the two
// brokers' AwaitTypedAnswer factoring (Phase 5 step 10) is meant to make
// this test suite pin down for both sides, not just the permission one.
struct Harness {
    rpc::EventBroadcaster broadcaster{"sess-1"};
    rpc::EventBroadcaster::Handle subscriber = broadcaster.Subscribe();
    rpc::PendingRequests pending;
    rpc::WireConfirmationBroker broker{"sess-1", broadcaster, pending};

    std::optional<v1::ServerEvent> Pop() { return broadcaster.Pop(subscriber); }
    std::size_t size() const { return broadcaster.queued(subscriber); }
};

// sessions.unattended_prompt_timeout_seconds set to a real, short bound.
struct TimeoutHarness {
    rpc::EventBroadcaster broadcaster{"sess-1"};
    rpc::EventBroadcaster::Handle subscriber = broadcaster.Subscribe();
    rpc::PendingRequests pending;
    rpc::WireConfirmationBroker broker{"sess-1", broadcaster, pending, 20ms};

    std::optional<v1::ServerEvent> Pop() { return broadcaster.Pop(subscriber); }
};

commands::ConfirmationRequest Question() {
    commands::ConfirmationRequest request;
    request.kind = "command_confirmation";
    request.title = "Delete the branch?";
    request.detail = "This cannot be undone.";
    request.options = {"yes", "no"};
    return request;
}

}  // namespace

TEST_CASE("a confirmation request round-trips to a chosen option", "[wire_confirmation]") {
    auto harness = std::make_shared<Harness>();

    auto answered =
        RunInBackground([harness] { return harness->broker.RequestConfirmation(Question()); });

    const auto event = harness->Pop();
    REQUIRE(event.has_value());
    REQUIRE(event->has_confirmation_request());
    const auto& request = event->confirmation_request();
    CHECK(event->session_id() == "sess-1");
    CHECK(request.kind() == "command_confirmation");
    CHECK(request.title() == "Delete the branch?");
    CHECK(request.detail() == "This cannot be undone.");
    REQUIRE(request.options_size() == 2);
    CHECK(request.options(0) == "yes");
    CHECK_FALSE(request.request_id().empty());

    CHECK_FALSE(answered.WaitFor(50ms));  // established: genuinely blocked

    CHECK(harness->pending.Resolve(request.request_id(), rpc::ConfirmationAnswer{"yes"}));

    REQUIRE(answered.Ready());
    const auto response = answered.Get();
    CHECK(response.answered());
    CHECK(response.chosen_option == "yes");
}

TEST_CASE("an unattended confirmation with a configured timeout answers empty rather than "
         "hanging forever",
         "[wire_confirmation][S5]") {
    auto harness = std::make_shared<TimeoutHarness>();

    auto answered =
        RunInBackground([harness] { return harness->broker.RequestConfirmation(Question()); });

    const auto event = harness->Pop();
    REQUIRE(event.has_value());

    REQUIRE(answered.Ready());  // the 20ms timeout elapses well inside the 2s deadline
    CHECK_FALSE(answered.Get().answered());
}

TEST_CASE("an answer arriving before the timeout still wins", "[wire_confirmation][S5]") {
    auto harness = std::make_shared<TimeoutHarness>();

    auto answered =
        RunInBackground([harness] { return harness->broker.RequestConfirmation(Question()); });

    const auto event = harness->Pop();
    REQUIRE(event.has_value());
    const auto& request = event->confirmation_request();

    CHECK(harness->pending.Resolve(request.request_id(), rpc::ConfirmationAnswer{"no"}));

    REQUIRE(answered.Ready());
    CHECK(answered.Get().chosen_option == "no");
}

TEST_CASE("an option the request never offered is refused", "[wire_confirmation]") {
    auto harness = std::make_shared<Harness>();
    auto answered =
        RunInBackground([harness] { return harness->broker.RequestConfirmation(Question()); });

    const auto event = harness->Pop();
    REQUIRE(event.has_value());
    const std::string id = event->confirmation_request().request_id();
    harness->pending.Resolve(id, rpc::ConfirmationAnswer{"maybe"});

    REQUIRE(answered.Ready());
    CHECK_FALSE(answered.Get().answered());
}

TEST_CASE("cancellation unblocks a waiting confirmation as unanswered", "[wire_confirmation]") {
    // Termination must not leave a worker parked on a confirmation nobody
    // will answer. Same reasoning as wire_permission_checker's equivalent
    // case: empty chosen_option is decided by "no usable answer arrived",
    // not by an accidental default.
    auto harness = std::make_shared<Harness>();
    auto answered =
        RunInBackground([harness] { return harness->broker.RequestConfirmation(Question()); });

    REQUIRE(harness->Pop().has_value());
    CHECK_FALSE(answered.WaitFor(50ms));

    harness->pending.Shutdown();

    REQUIRE(answered.Ready());
    CHECK_FALSE(answered.Get().answered());
}

TEST_CASE("a permission answer delivered against a confirmation id fails structurally",
         "[wire_confirmation]") {
    // The typed variant makes cross-delivery a type mismatch, not a parse
    // outcome the broker had to remember to guard: a PermissionAnswer can
    // never be read as a ConfirmationAnswer, so the wrong-kind answer fails
    // closed before any option-matching happens at all.
    auto harness = std::make_shared<Harness>();
    auto answered =
        RunInBackground([harness] { return harness->broker.RequestConfirmation(Question()); });

    const auto event = harness->Pop();
    REQUIRE(event.has_value());
    harness->pending.Resolve(event->confirmation_request().request_id(),
                             rpc::PermissionAnswer{tools::PermissionDecision::Allow, {}});

    REQUIRE(answered.Ready());
    CHECK_FALSE(answered.Get().answered());
}

TEST_CASE("each request gets its own correlation id", "[wire_confirmation]") {
    auto harness = std::make_shared<Harness>();

    auto first =
        RunInBackground([harness] { return harness->broker.RequestConfirmation(Question()); });
    const auto first_event = harness->Pop();
    REQUIRE(first_event.has_value());
    const std::string first_id = first_event->confirmation_request().request_id();
    harness->pending.Resolve(first_id, rpc::ConfirmationAnswer{"yes"});
    REQUIRE(first.Ready());

    auto second =
        RunInBackground([harness] { return harness->broker.RequestConfirmation(Question()); });
    const auto second_event = harness->Pop();
    REQUIRE(second_event.has_value());
    const std::string second_id = second_event->confirmation_request().request_id();
    CHECK(second_id != first_id);
    harness->pending.Resolve(second_id, rpc::ConfirmationAnswer{"no"});
    REQUIRE(second.Ready());
}
