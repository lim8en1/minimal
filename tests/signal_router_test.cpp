#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "deadline.hpp"
#include "fakes.hpp"
#include "minimal/agent/session.hpp"
#include "minimal/agent/signal_router.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/rpc/session_wiring.hpp"

using namespace minimal;
using namespace std::chrono_literals;

namespace {

// A live Session is a worker thread plus a loop, so delivery tests need the
// same scaffolding session_test.cpp's fixture builds -- trimmed to what a
// signal can observe: the transcript the injection lands in, and the idle
// wake. Built per test; destroyed before the borrowed registries.
struct RouterSessionFixture {
    RouterSessionFixture() : observers(common::DefaultLogger()) {
        preset.name = "test";
        preset.allowed_tools = {"*"};
        preset.permission_mode = testing::AllowAllPolicy();
        preset.run_mode = tools::PermissionRunMode::Normal;
    }

    void Start(std::string session_id) {
        auto owned_provider = std::make_unique<testing::FakeLlmProvider>();
        provider = owned_provider.get();
        for (auto& step : scripted) owned_provider->PushResponse(step.first, step.second);
        auto owned_store = std::make_unique<testing::InMemoryHistoryStore>();
        store = owned_store.get();

        agent::SessionParams params;
        params.session_id = std::move(session_id);
        params.name = "router test session";
        params.agent_preset = "test";
        params.project_root = std::filesystem::temp_directory_path();
        params.wake_on_injection = wake_on_injection;
        rpc::FinishSessionWiring(params);

        session = std::make_shared<agent::Session>(std::move(params), preset,
                                                   std::move(owned_provider),
                                                   std::move(owned_store), tools_registry,
                                                   agent::LoopConfig{}, observers,
                                                   common::DefaultLogger());
    }

    void Script(const std::string& text) {
        scripted.emplace_back(testing::TextResponse(text), std::vector<llm::StreamEvent>{});
    }

    ~RouterSessionFixture() {
        if (session) session->Terminate();
    }

    agent::AgentPreset preset;
    tools::ToolRegistry tools_registry;
    agent::SessionObserverRegistry observers;
    std::vector<std::pair<llm::ChatResponse, std::vector<llm::StreamEvent>>> scripted;
    bool wake_on_injection = true;
    testing::FakeLlmProvider* provider = nullptr;
    testing::InMemoryHistoryStore* store = nullptr;  // owned by the session
    std::shared_ptr<agent::Session> session;
};

// Polls the transcript until the needle text appears. The settle condition
// is the TRANSCRIPT, not just idle: WaitForIdleForTesting returns true while
// a woken turn has not yet STARTED (idle predicate), so a test that checks
// the transcript right after idle can race the turn it just triggered.
bool WaitForInTranscript(testing::InMemoryHistoryStore* store, const std::string& needle,
                         std::chrono::milliseconds timeout = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        for (const auto& message : store->messages) {
            if (message.role != llm::Role::User) continue;
            for (const auto& block : message.content) {
                if (auto* text = std::get_if<llm::TextBlock>(&block)) {
                    if (text->text.find(needle) != std::string::npos) return true;
                }
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

// Restricted "ci" with two declared signals; extended "reminder"; both enabled.
config::ChannelEntry CiChannel() {
    config::ChannelEntry entry;
    entry.name = "ci";
    entry.signals = {"build.completed", "build.failed"};
    entry.command = "true";
    return entry;
}

config::ChannelEntry ReminderChannel() {
    config::ChannelEntry entry;
    entry.name = "reminder";
    entry.extended = true;
    entry.command = "true";
    return entry;
}

}  // namespace

TEST_CASE("WrapSignal: provenance frame, attribute sanitization", "[signals][router]") {
    const std::string wrapped = agent::WrapSignal(
        "ci", "build.completed", {{"run_id", "123"}, {"bad key!", "dropped"}}, "build passed");
    CHECK(wrapped ==
          "<signal source=\"ci\" signal=\"build.completed\" run_id=\"123\">\n"
          "build passed\n"
          "</signal>");

    // A value cannot break out of its attribute: quotes are escaped, control
    // characters dropped, the rest verbatim.
    const std::string escaped = agent::WrapSignal(
        "ci", "build.failed", {{"msg", "said \"hi\"\\\nnewline"}}, "x");
    CHECK(escaped.find("msg=\"said \\\"hi\\\"\\\\newline\"") != std::string::npos);

    // The internal from= attribute carries an uppercase ULID: keys are
    // filtered, values are verbatim.
    CHECK(agent::WrapSignal("sessions", "", {{"from", "01ARZ3NDEKTSV4RRFFQ69G5FAV"}}, "") ==
          "<signal source=\"sessions\" from=\"01ARZ3NDEKTSV4RRFFQ69G5FAV\"></signal>");
}

TEST_CASE("CheckSelector: syntax and mode rules", "[signals][router]") {
    agent::SignalRouter router;
    router.RegisterChannel(CiChannel());
    router.RegisterChannel(ReminderChannel());

    std::string error;
    std::string channel;

    // Restricted: per-signal, declared only.
    REQUIRE(router.CheckSelector("ci/build.completed", error, &channel));
    CHECK(channel == "ci");
    CHECK_FALSE(router.CheckSelector("ci", error));
    CHECK(error.find("restricted") != std::string::npos);
    CHECK_FALSE(router.CheckSelector("ci/build.unknown", error));
    CHECK(error.find("not declared") != std::string::npos);

    // Extended: channel-level only.
    REQUIRE(router.CheckSelector("reminder", error, &channel));
    CHECK(channel == "reminder");
    CHECK_FALSE(router.CheckSelector("reminder/anything", error));
    CHECK(error.find("extended") != std::string::npos);

    // Internal source: channel-level only.
    REQUIRE(router.CheckSelector("sessions", error, &channel));
    CHECK(channel == "sessions");
    CHECK_FALSE(router.CheckSelector("sessions/message", error));

    // Syntax: empty channel, bad signal charset.
    CHECK_FALSE(router.CheckSelector("", error));
    CHECK_FALSE(router.CheckSelector("ci/build failed", error));
    CHECK_FALSE(router.CheckSelector("ci/BUILD", error));

    // Unknown channel.
    CHECK_FALSE(router.CheckSelector("nowhere/x", error));
    CHECK(error.find("unknown signal channel") != std::string::npos);
}

TEST_CASE("DeliverInternal: addressed delivery, subscription-gated", "[signals][router]") {
    agent::SignalRouter router;
    RouterSessionFixture alice;
    alice.Script("ack");
    alice.Start("alice");
    RouterSessionFixture bob;
    bob.Script("ack");
    bob.Start("bob");

    std::string error;
    // Bob subscribed; alice did not. Subscription is the whole receiver gate.
    REQUIRE(router.Subscribe(bob.session, "sessions", error));

    // To bob: delivered. Alice never subscribed, and must not receive it even
    // though she is running.
    CHECK(router.DeliverInternal("alice", "bob", "please review PR 7") ==
          agent::SignalDelivery::kDelivered);
    // Self-address refused at the router (second guard behind the tool's).
    CHECK(router.DeliverInternal("bob", "bob", "note to self") ==
          agent::SignalDelivery::kInvalid);
    // Unknown target / unsubscribed target: dropped, not queued.
    CHECK(router.DeliverInternal("bob", "nobody", "hello") ==
          agent::SignalDelivery::kNoSubscriber);
    CHECK(router.DeliverInternal("bob", "alice", "hello") ==
          agent::SignalDelivery::kNoSubscriber);
    // Empty anything: invalid.
    CHECK(router.DeliverInternal("bob", "alice", "") == agent::SignalDelivery::kInvalid);

    // The delivered signal is the provenance-wrapped USER message the wake
    // path treats as bob's turn content.
    CHECK(WaitForInTranscript(bob.store, "<signal source=\"sessions\" from=\"alice\">"));
    CHECK(WaitForInTranscript(bob.store, "please review PR 7"));
}

TEST_CASE("DeliverInternal wakes an idle subscribed session", "[signals][router][wake]") {
    agent::SignalRouter router;
    RouterSessionFixture carol;
    carol.Script("on it");
    carol.Start("carol");
    std::string error;
    REQUIRE(router.Subscribe(carol.session, "sessions", error));

    // Idle, no user turn submitted: the injection IS the turn.
    CHECK(router.DeliverInternal("dave", "carol", "deploy finished") ==
          agent::SignalDelivery::kDelivered);
    CHECK(WaitForInTranscript(carol.store, "deploy finished"));
}

TEST_CASE("OnMcpNotification: wire contract, allowlist, meta sanitization", "[signals][router]") {
    agent::SignalRouter router;
    router.RegisterChannel(CiChannel());
    RouterSessionFixture eve;
    eve.Script("ack");
    eve.Start("eve");
    std::string error;
    REQUIRE(router.Subscribe(eve.session, "ci/build.completed", error));

    auto notify = [&](const std::string& raw) { router.OnMcpNotification("ci", raw); };

    // The happy frame: right method, declared signal, meta sanitized.
    notify(nlohmann::json{
        {"jsonrpc", "2.0"},
        {"method", "notifications/minimal/signal"},
        {"params",
         {{"signal", "build.completed"},
          {"content", "build 123 passed"},
          {"meta", {{"run_id", "123"}, {"bad-key", "dropped"}}}}}}
               .dump());

    // Not a signal frame: another server notification is ignored silently.
    notify(nlohmann::json{{"jsonrpc", "2.0"},
                          {"method", "notifications/tools/list_changed"}}
               .dump());
    // Malformed: not JSON, missing params, missing fields, bad signal name.
    notify("not json at all");
    notify(nlohmann::json{{"jsonrpc", "2.0"},
                          {"method", "notifications/minimal/signal"}}
               .dump());
    notify(nlohmann::json{{"jsonrpc", "2.0"},
                          {"method", "notifications/minimal/signal"},
                          {"params", {{"signal", "build.completed"}}}}
               .dump());
    notify(nlohmann::json{{"jsonrpc", "2.0"},
                          {"method", "notifications/minimal/signal"},
                          {"params", {{"signal", "not declared"}, {"content", "x"}}}}
               .dump());
    notify(nlohmann::json{{"jsonrpc", "2.0"},
                          {"method", "notifications/minimal/signal"},
                          {"params", {{"signal", "BAD NAME!"}, {"content", "x"}}}}
               .dump());

    // Exactly ONE turn (the delivered signal) reached the session; every
    // malformed or undeclared frame was dropped.
    CHECK(WaitForInTranscript(eve.store, "build 123 passed"));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    int signal_turns = 0;
    for (const auto& message : eve.store->messages) {
        if (message.role != llm::Role::User) continue;
        for (const auto& block : message.content) {
            if (auto* text = std::get_if<llm::TextBlock>(&block)) {
                if (text->text.find("build 123 passed") != std::string::npos) ++signal_turns;
            }
        }
    }
    CHECK(signal_turns == 1);
}

TEST_CASE("OnMcpNotification: a full injection queue refuses delivery without crashing",
          "[signals][router]") {
    // THE kDropped PATH: a subscribed, LIVE session whose InjectionQueue is
    // already at kMaxPending is a different outcome from "no subscriber at
    // all" (kNoSubscriber) -- Deliver() -> Session::QueueInjectedMessage
    // refuses every target, and SignalRouter::OnMcpNotification's switch
    // must handle Delivery::kDropped without throwing (it previously fell
    // through silently; this test exercises the branch added alongside the
    // fix). No log-capturing harness exists in this test tree, so this
    // confirms behavior (no crash, no phantom delivery) rather than the log
    // line's exact text.
    agent::SignalRouter router;
    router.RegisterChannel(CiChannel());
    RouterSessionFixture gina;
    gina.wake_on_injection = false;  // keep injections staged, never auto-flushed
    gina.Start("gina");
    std::string error;
    REQUIRE(router.Subscribe(gina.session, "ci/build.completed", error));

    // Fill the queue to exactly kMaxPending via the same entry point a
    // subagent completion or background task uses -- ground truth for "the
    // queue is genuinely full", not an internals assumption.
    for (std::size_t i = 0; i < agent::InjectionQueue::kMaxPending; ++i) {
        REQUIRE(gina.session->QueueInjectedMessage(
            llm::Message{llm::Role::User, {llm::TextBlock{"filler"}}}));
    }
    REQUIRE_FALSE(gina.session->QueueInjectedMessage(
        llm::Message{llm::Role::User, {llm::TextBlock{"one too many"}}}));

    // The notification itself must not throw, and must not silently make
    // room -- the queue is still exactly full afterward (a probe push still
    // refuses), confirming the signal's content never entered.
    REQUIRE_NOTHROW(router.OnMcpNotification(
        "ci", nlohmann::json{{"jsonrpc", "2.0"},
                            {"method", "notifications/minimal/signal"},
                            {"params",
                             {{"signal", "build.completed"}, {"content", "should be dropped"}}}}
                       .dump()));
    CHECK_FALSE(gina.session->QueueInjectedMessage(
        llm::Message{llm::Role::User, {llm::TextBlock{"still full"}}}));
}

TEST_CASE("restricted channel: undeclared signals are loud drops, extended pass everything",
          "[signals][router]") {
    agent::SignalRouter router;
    router.RegisterChannel(CiChannel());
    router.RegisterChannel(ReminderChannel());

    RouterSessionFixture frank;
    frank.Script("ack");
    frank.Script("ack");
    frank.Start("frank");
    std::string error;
    // Frank subscribed to the WHOLE extended channel and one ci signal.
    REQUIRE(router.Subscribe(frank.session, "reminder", error));
    REQUIRE(router.Subscribe(frank.session, "ci/build.failed", error));

    // One delivery at a time, settling on the TRANSCRIPT between: a signal
    // landing while the wake turn is still running queues for the NEXT
    // boundary, which would never come and would make this test lie.

    router.OnMcpNotification("reminder",
                             nlohmann::json{{"method", "notifications/minimal/signal"},
                                            {"params", {{"signal", "any.thing"},
                                                        {"content", "extended passes"}}}}
                                 .dump());
    CHECK(WaitForInTranscript(frank.store, "extended passes"));
    router.OnMcpNotification("ci",
                             nlohmann::json{{"method", "notifications/minimal/signal"},
                                            {"params", {{"signal", "build.failed"},
                                                        {"content", "restricted exact match"}}}}
                                 .dump());
    CHECK(WaitForInTranscript(frank.store, "restricted exact match"));
    // Frank's OTHER ci subscription does not exist: build.completed must not
    // reach him (he never subscribed to it). Dropped, so nothing to settle.
    router.OnMcpNotification("ci",
                             nlohmann::json{{"method", "notifications/minimal/signal"},
                                            {"params", {{"signal", "build.completed"},
                                                        {"content", "not for frank"}}}}
                                 .dump());

    int extended = 0, restricted = 0, not_for_frank = 0;
    for (const auto& message : frank.store->messages) {
        if (message.role != llm::Role::User) continue;
        for (const auto& block : message.content) {
            if (auto* text = std::get_if<llm::TextBlock>(&block)) {
                if (text->text.find("extended passes") != std::string::npos) ++extended;
                if (text->text.find("restricted exact match") != std::string::npos) ++restricted;
                if (text->text.find("not for frank") != std::string::npos) ++not_for_frank;
            }
        }
    }
    CHECK(extended == 1);
    CHECK(restricted == 1);
    CHECK(not_for_frank == 0);
}

TEST_CASE("Unsubscribe stops delivery; resume semantics replace the whole set",
          "[signals][router]") {
    agent::SignalRouter router;
    RouterSessionFixture gina;
    gina.Script("ack");
    gina.Start("gina");
    std::string error;
    REQUIRE(router.Subscribe(gina.session, "sessions", error));
    router.Unsubscribe("gina");
    CHECK(router.DeliverInternal("someone", "gina", "hello") ==
          agent::SignalDelivery::kNoSubscriber);
}

TEST_CASE("SignalContextFor: joined instructions for subscribed channels", "[signals][router]") {
    agent::SignalRouter router;
    router.RegisterChannel(CiChannel());
    router.RegisterChannel(ReminderChannel());
    router.SetChannelInstructions("ci", "Build events: read logs before acting.");
    CHECK(router.SignalContextFor({"ci/build.completed"}) ==
          "Channel 'ci': Build events: read logs before acting.");
    CHECK(router.SignalContextFor({"reminder"}).empty());  // none captured
    CHECK(router.SignalContextFor({"nowhere"}).empty());

    router.SetChannelInstructions("reminder", "Reminders are self-contained.");
    const std::string both = router.SignalContextFor({"ci/build.failed", "reminder"});
    REQUIRE(both.find("Channel 'ci':") != std::string::npos);
    REQUIRE(both.find("Channel 'reminder':") != std::string::npos);
}
