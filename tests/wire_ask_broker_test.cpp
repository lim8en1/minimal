#include <chrono>
#include <memory>
#include <optional>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "deadline.hpp"
#include "minimal/rpc/wire_ask_broker.hpp"

using namespace minimal;
using minimal::testing::RunInBackground;
using namespace std::chrono_literals;

namespace {

// The three objects an ask round-trip needs -- mirrors
// wire_confirmation_broker_test.cpp's Harness, since both run on WirePrompt.
struct Harness {
    rpc::EventBroadcaster broadcaster{"sess-1"};
    rpc::EventBroadcaster::Handle subscriber = broadcaster.Subscribe();
    rpc::PendingRequests pending;
    rpc::WireAskBroker broker{"sess-1", broadcaster, pending};

    std::optional<v1::ServerEvent> Pop() { return broadcaster.Pop(subscriber); }
};

struct TimeoutHarness {
    rpc::EventBroadcaster broadcaster{"sess-1"};
    rpc::EventBroadcaster::Handle subscriber = broadcaster.Subscribe();
    rpc::PendingRequests pending;
    rpc::WireAskBroker broker{"sess-1", broadcaster, pending, 20ms};

    std::optional<v1::ServerEvent> Pop() { return broadcaster.Pop(subscriber); }
};

tools::AskRequest Question() {
    tools::AskRequest request;
    tools::AskQuestion question;
    question.question = "Which approach?";
    question.header = "Approach";
    question.options.push_back({"rewrite", "Replace the module wholesale"});
    question.options.push_back({"patch", "Fix it in place"});
    request.questions.push_back(std::move(question));
    return request;
}

rpc::AskUserAnswer Pick(std::vector<std::string> labels) {
    rpc::AskUserAnswer answer;
    answer.answered = true;
    answer.answers.push_back({std::move(labels), {}});
    return answer;
}

}  // namespace

TEST_CASE("an ask round-trips to the selected labels", "[wire_ask]") {
    auto harness = std::make_shared<Harness>();

    auto answered = RunInBackground([harness] { return harness->broker.Ask(Question()); });

    const auto event = harness->Pop();
    REQUIRE(event.has_value());
    REQUIRE(event->has_ask_request());
    CHECK(event->session_id() == "sess-1");
    const auto& wire = event->ask_request();
    REQUIRE(wire.questions_size() == 1);
    CHECK(wire.questions(0).question() == "Which approach?");
    CHECK(wire.questions(0).header() == "Approach");
    REQUIRE(wire.questions(0).options_size() == 2);
    CHECK(wire.questions(0).options(0).label() == "rewrite");
    CHECK(wire.questions(0).options(0).description() == "Replace the module wholesale");
    CHECK_FALSE(wire.questions(0).multiple());
    CHECK(wire.questions(0).allow_other());
    CHECK_FALSE(wire.request_id().empty());

    CHECK_FALSE(answered.WaitFor(50ms));  // genuinely blocked, not politely looping

    CHECK(harness->pending.Resolve(wire.request_id(), Pick({"patch"})));

    REQUIRE(answered.Ready());
    const auto response = answered.Get();
    REQUIRE(response.answered);
    REQUIRE(response.slots.size() == 1);
    REQUIRE(response.slots[0].selected.size() == 1);
    CHECK(response.slots[0].selected[0] == "patch");
    CHECK(response.slots[0].other.empty());
}

TEST_CASE("an unanswered ask (dismissed) carries answered=false, not empty selections",
          "[wire_ask]") {
    auto harness = std::make_shared<Harness>();
    auto answered = RunInBackground([harness] { return harness->broker.Ask(Question()); });

    const auto event = harness->Pop();
    REQUIRE(event.has_value());

    rpc::AskUserAnswer declined;
    declined.answered = false;
    CHECK(harness->pending.Resolve(event->ask_request().request_id(), declined));

    REQUIRE(answered.Ready());
    CHECK_FALSE(answered.Get().answered);
    // The tool layer formats this as "dismissed"; the broker hands the flag
    // through untouched rather than fabricating slots for it.
    CHECK(answered.Get().slots.empty());
}

TEST_CASE("a label the question never offered is refused", "[wire_ask]") {
    auto harness = std::make_shared<Harness>();
    auto answered = RunInBackground([harness] { return harness->broker.Ask(Question()); });

    const auto event = harness->Pop();
    REQUIRE(event.has_value());
    harness->pending.Resolve(event->ask_request().request_id(), Pick({"maybe"}));

    REQUIRE(answered.Ready());
    REQUIRE(answered.Get().answered);
    // The answer struct is answered, but the invalid slot stays empty: the
    // model reads "the user did not answer", never an invented label.
    REQUIRE(answered.Get().slots.size() == 1);
    CHECK(answered.Get().slots[0].selected.empty());
}

TEST_CASE("multi-select rejects two picks on a single-select question", "[wire_ask]") {
    auto harness = std::make_shared<Harness>();
    auto answered = RunInBackground([harness] { return harness->broker.Ask(Question()); });

    const auto event = harness->Pop();
    REQUIRE(event.has_value());
    harness->pending.Resolve(event->ask_request().request_id(), Pick({"rewrite", "patch"}));

    REQUIRE(answered.Ready());
    CHECK(answered.Get().slots[0].selected.empty());
}

TEST_CASE("multi-select accepts several picks on a multiple question", "[wire_ask]") {
    tools::AskRequest request;
    tools::AskQuestion question;
    question.question = "Which tests?";
    question.multiple = true;
    question.options.push_back({"unit"});
    question.options.push_back({"e2e"});
    request.questions.push_back(std::move(question));

    auto harness = std::make_shared<Harness>();
    auto answered = RunInBackground([harness, request] { return harness->broker.Ask(request); });

    const auto event = harness->Pop();
    REQUIRE(event.has_value());
    CHECK(event->ask_request().questions(0).multiple());
    harness->pending.Resolve(event->ask_request().request_id(), Pick({"unit", "e2e"}));

    REQUIRE(answered.Ready());
    REQUIRE(answered.Get().slots[0].selected.size() == 2);
}

TEST_CASE("free text is accepted only when the question allowed it, and never mixed "
          "with a selection",
          "[wire_ask]") {
    auto harness = std::make_shared<Harness>();

    SECTION("allowed: the free text lands in other") {
        auto answered =
            RunInBackground([harness] { return harness->broker.Ask(Question()); });
        const auto event = harness->Pop();
        REQUIRE(event.has_value());

        rpc::AskUserAnswer answer;
        answer.answered = true;
        answer.answers.push_back(rpc::AskUserAnswer::Slot{{}, "neither, use a config flag"});
        CHECK(harness->pending.Resolve(event->ask_request().request_id(), answer));

        REQUIRE(answered.Ready());
        CHECK(answered.Get().slots[0].other == "neither, use a config flag");
        CHECK(answered.Get().slots[0].selected.empty());
    }

    SECTION("disallowed: the slot is unanswered") {
        tools::AskRequest request = Question();
        request.questions[0].allow_other = false;
        auto answered = RunInBackground([harness, request] { return harness->broker.Ask(request); });
        const auto event = harness->Pop();
        REQUIRE(event.has_value());
        CHECK_FALSE(event->ask_request().questions(0).allow_other());

        rpc::AskUserAnswer answer;
        answer.answered = true;
        answer.answers.push_back(rpc::AskUserAnswer::Slot{{}, "neither"});
        CHECK(harness->pending.Resolve(event->ask_request().request_id(), answer));

        REQUIRE(answered.Ready());
        CHECK(answered.Get().slots[0].other.empty());
    }

    SECTION("mixed: selection plus text is rejected wholesale") {
        auto answered =
            RunInBackground([harness] { return harness->broker.Ask(Question()); });
        const auto event = harness->Pop();
        REQUIRE(event.has_value());

        rpc::AskUserAnswer answer;
        answer.answered = true;
        answer.answers.push_back(rpc::AskUserAnswer::Slot{{"patch"}, "plus a note"});
        CHECK(harness->pending.Resolve(event->ask_request().request_id(), answer));

        REQUIRE(answered.Ready());
        CHECK(answered.Get().slots[0].other.empty());
        CHECK(answered.Get().slots[0].selected.empty());
    }
}

TEST_CASE("answers align to their questions by index", "[wire_ask]") {
    tools::AskRequest request;
    tools::AskQuestion first;
    first.question = "q1";
    first.options.push_back({"a"});
    tools::AskQuestion second;
    second.question = "q2";
    second.options.push_back({"b"});
    request.questions.push_back(std::move(first));
    request.questions.push_back(std::move(second));

    auto harness = std::make_shared<Harness>();
    auto answered = RunInBackground([harness, request] { return harness->broker.Ask(request); });

    const auto event = harness->Pop();
    REQUIRE(event.has_value());
    REQUIRE(event->ask_request().questions_size() == 2);

    rpc::AskUserAnswer answer;
    answer.answered = true;
    answer.answers.push_back(rpc::AskUserAnswer::Slot{{"a"}, {}});
    answer.answers.push_back(rpc::AskUserAnswer::Slot{{"b"}, {}});
    CHECK(harness->pending.Resolve(event->ask_request().request_id(), answer));

    REQUIRE(answered.Ready());
    REQUIRE(answered.Get().slots.size() == 2);
    CHECK(answered.Get().slots[0].selected.size() == 1);
    CHECK(answered.Get().slots[1].selected.size() == 1);
}

TEST_CASE("an unattended ask with a configured timeout returns unanswered", "[wire_ask][S5]") {
    TimeoutHarness harness;
    auto answered = RunInBackground([&harness] { return harness.broker.Ask(Question()); });

    REQUIRE(harness.Pop().has_value());
    REQUIRE(answered.Ready());  // the 20ms timeout elapses well inside the 2s deadline
    CHECK_FALSE(answered.Get().answered);
}

TEST_CASE("cancellation unblocks a waiting ask as unanswered", "[wire_ask]") {
    auto harness = std::make_shared<Harness>();
    auto answered = RunInBackground([harness] { return harness->broker.Ask(Question()); });

    REQUIRE(harness->Pop().has_value());
    CHECK_FALSE(answered.WaitFor(50ms));

    harness->pending.Shutdown();

    REQUIRE(answered.Ready());
    CHECK_FALSE(answered.Get().answered);
}

TEST_CASE("a confirmation answer delivered against an ask id fails structurally", "[wire_ask]") {
    auto harness = std::make_shared<Harness>();
    auto answered = RunInBackground([harness] { return harness->broker.Ask(Question()); });

    const auto event = harness->Pop();
    REQUIRE(event.has_value());
    harness->pending.Resolve(event->ask_request().request_id(), rpc::ConfirmationAnswer{"rewrite"});

    REQUIRE(answered.Ready());
    CHECK_FALSE(answered.Get().answered);
}
