// WirePluginPromptBroker: the opaque plugin-prompt transport. It knows no
// prompt shape -- the payload goes out verbatim and the answer comes back
// parsed but unvalidated (the ask's slot validation is tool_ask_test.cpp's).
// What it owns is the outcome: answered/dismissed/unavailable from the
// client, timed-out and cancelled from the wait itself, and fail-closed
// dismissal for an answer it cannot read.

#include <chrono>
#include <memory>
#include <optional>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "deadline.hpp"
#include "minimal/rpc/wire_plugin_prompt_broker.hpp"

using namespace minimal;
using minimal::testing::RunInBackground;
using namespace std::chrono_literals;

namespace {

// The three objects a prompt round-trip needs -- mirrors
// wire_confirmation_broker_test.cpp's Harness, since both run on WirePrompt.
struct Harness {
    rpc::EventBroadcaster broadcaster{"sess-1"};
    rpc::EventBroadcaster::Handle subscriber = broadcaster.Subscribe();
    rpc::PendingRequests pending;
    rpc::WirePluginPromptBroker broker{"sess-1", broadcaster, pending};

    std::optional<v1::ServerEvent> Pop() { return broadcaster.Pop(subscriber); }
};

struct TimeoutHarness {
    rpc::EventBroadcaster broadcaster{"sess-1"};
    rpc::EventBroadcaster::Handle subscriber = broadcaster.Subscribe();
    rpc::PendingRequests pending;
    rpc::WirePluginPromptBroker broker{"sess-1", broadcaster, pending, 20ms};

    std::optional<v1::ServerEvent> Pop() { return broadcaster.Pop(subscriber); }
};

tools::PromptRequest Request() {
    return {"ask", nlohmann::json{{"questions", nlohmann::json::array({"q"})}}};
}

rpc::PluginPromptAnswer Reply(tools::PromptOutcome outcome, std::string json = {}) {
    rpc::PluginPromptAnswer answer;
    answer.outcome = outcome;
    answer.answer_json = std::move(json);
    return answer;
}

}  // namespace

TEST_CASE("a plugin prompt carries its kind and payload verbatim and returns the parsed "
          "answer",
          "[wire_prompt]") {
    auto harness = std::make_shared<Harness>();
    auto answered = RunInBackground([harness] { return harness->broker.Prompt(Request()); });

    const auto event = harness->Pop();
    REQUIRE(event.has_value());
    REQUIRE(event->has_plugin_prompt());
    CHECK(event->session_id() == "sess-1");
    const auto& wire = event->plugin_prompt();
    CHECK(wire.kind() == "ask");
    CHECK(nlohmann::json::parse(wire.payload_json()) == Request().payload);
    CHECK_FALSE(wire.request_id().empty());

    CHECK_FALSE(answered.WaitFor(50ms));  // genuinely blocked, not politely looping

    CHECK(harness->pending.Resolve(
        wire.request_id(), Reply(tools::PromptOutcome::Answered, R"({"answers":[1]})")));

    REQUIRE(answered.Ready());
    const auto result = answered.Get();
    CHECK(result.outcome == tools::PromptOutcome::Answered);
    CHECK(result.answer == nlohmann::json{{"answers", {1}}});
}

TEST_CASE("the client's outcome passes through untouched", "[wire_prompt]") {
    for (const auto outcome : {tools::PromptOutcome::Dismissed, tools::PromptOutcome::Unavailable}) {
        auto harness = std::make_shared<Harness>();
        auto answered = RunInBackground([harness] { return harness->broker.Prompt(Request()); });
        const auto event = harness->Pop();
        REQUIRE(event.has_value());
        harness->pending.Resolve(event->plugin_prompt().request_id(), Reply(outcome));
        REQUIRE(answered.Ready());
        CHECK(answered.Get().outcome == outcome);
        CHECK(answered.Get().answer.is_null());
    }
}

TEST_CASE("an answer the broker cannot read is a dismissal, never an answer",
          "[wire_prompt]") {
    auto harness = std::make_shared<Harness>();
    auto answered = RunInBackground([harness] { return harness->broker.Prompt(Request()); });
    const auto event = harness->Pop();
    REQUIRE(event.has_value());

    std::string json;
    SECTION("unparseable") { json = "{not json"; }
    SECTION("over the size cap") {
        json = "\"" + std::string(rpc::WirePluginPromptBroker::kMaxAnswerBytes, 'x') + "\"";
    }
    harness->pending.Resolve(event->plugin_prompt().request_id(),
                             Reply(tools::PromptOutcome::Answered, json));
    REQUIRE(answered.Ready());
    CHECK(answered.Get().outcome == tools::PromptOutcome::Dismissed);
    CHECK(answered.Get().answer.is_null());
}

TEST_CASE("an unattended prompt with a configured timeout returns TimedOut",
          "[wire_prompt][S5]") {
    TimeoutHarness harness;
    auto answered = RunInBackground([&harness] { return harness.broker.Prompt(Request()); });

    REQUIRE(harness.Pop().has_value());
    REQUIRE(answered.Ready());  // the 20ms timeout elapses well inside the 2s deadline
    CHECK(answered.Get().outcome == tools::PromptOutcome::TimedOut);
}

TEST_CASE("cancellation unblocks a waiting prompt as Cancelled", "[wire_prompt]") {
    auto harness = std::make_shared<Harness>();
    auto answered = RunInBackground([harness] { return harness->broker.Prompt(Request()); });

    REQUIRE(harness->Pop().has_value());
    CHECK_FALSE(answered.WaitFor(50ms));

    SECTION("turn cancel") { harness->pending.CancelAll(); }
    SECTION("session terminate") { harness->pending.Shutdown(); }

    REQUIRE(answered.Ready());
    CHECK(answered.Get().outcome == tools::PromptOutcome::Cancelled);
}

TEST_CASE("a cross-delivered answer of another prompt type is not an answer",
          "[wire_prompt]") {
    auto harness = std::make_shared<Harness>();
    auto answered = RunInBackground([harness] { return harness->broker.Prompt(Request()); });
    const auto event = harness->Pop();
    REQUIRE(event.has_value());
    harness->pending.Resolve(event->plugin_prompt().request_id(),
                             rpc::ConfirmationAnswer{"yes"});
    REQUIRE(answered.Ready());
    CHECK(answered.Get().outcome != tools::PromptOutcome::Answered);
}
