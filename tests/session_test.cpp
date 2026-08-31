#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "deadline.hpp"
#include "fakes.hpp"
#include "minimal/agent/session.hpp"
#include "minimal/common/logger.hpp"
#include "temp_dir.hpp"

#include "bash_session_state.hpp"
#include "bash_settings.hpp"
#include "bash_tool.hpp"

using namespace minimal;
using namespace std::chrono_literals;

namespace {

// A session is a live object with a worker thread, so every test needs the same
// scaffolding. Bundled rather than repeated, and destroyed in one place -- the
// Session must be torn down before the registries it borrows.
struct SessionFixture {
    explicit SessionFixture(std::filesystem::path root = std::filesystem::temp_directory_path())
        : project_root(std::move(root)), observers(common::DefaultLogger()) {
        preset.name = "test";
        preset.allowed_tools = {"*"};
        preset.permission_mode = testing::AllowAllPolicy();
        preset.run_mode = tools::PermissionRunMode::Normal;
    }

    // Built last, so callers can script the provider and register tools first.
    void Start(std::string session_id = "sess-1") {
        auto owned_provider = std::make_unique<testing::FakeLlmProvider>();
        provider = owned_provider.get();
        for (auto& step : scripted) {
            owned_provider->PushResponse(step.first, step.second);
        }

        // The fake store, not SqliteHistoryStore: these tests are about the
        // three-thread topology and the turn lifecycle, and a database would add
        // a file, a schema and DB-minted ids to every one of them without
        // testing anything they are about. Persistence through a live session is
        // covered against the real store in session_service_test.cpp.
        auto owned_store = std::make_unique<testing::InMemoryHistoryStore>();
        store = owned_store.get();

        agent::SessionParams params;
        params.session_id = std::move(session_id);
        params.name = "test session";
        params.agent_preset = "test";
        params.project_root = project_root;
        params.seed_grants = seed_grants;
        params.policy_ceiling = policy_ceiling;
        params.on_turn_finished = on_turn_finished;

        session = std::make_unique<agent::Session>(
            std::move(params), preset, std::move(owned_provider), std::move(owned_store),
            tools_registry, loop_config, observers, common::DefaultLogger());
    }

    void Script(llm::ChatResponse response, std::vector<llm::StreamEvent> events = {}) {
        scripted.emplace_back(std::move(response), std::move(events));
    }

    // Drains everything currently queued. The writer thread's job, done inline.
    std::vector<v1::ServerEvent> DrainQueue() {
        std::vector<v1::ServerEvent> out;
        while (auto event = session->queue().TryPop()) out.push_back(std::move(*event));
        return out;
    }

    ~SessionFixture() {
        if (session) session->Terminate();
    }

    std::filesystem::path project_root;
    agent::AgentPreset preset;
    agent::LoopConfig loop_config;
    tools::ToolRegistry tools_registry;
    agent::SessionObserverRegistry observers;
    std::vector<std::pair<llm::ChatResponse, std::vector<llm::StreamEvent>>> scripted;
    // Subagent plumbing, forwarded to SessionParams at Start().
    std::vector<std::string> seed_grants;
    std::optional<tools::PermissionPolicy> policy_ceiling;
    std::function<void(const agent::TurnResult&)> on_turn_finished;
    testing::FakeLlmProvider* provider = nullptr;
    testing::InMemoryHistoryStore* store = nullptr;  // owned by the session
    std::unique_ptr<agent::Session> session;
};

// Drains until a PermissionRequest shows up, returning its request_id. The
// worker is parked on that prompt by the time this returns, which is the state
// the caller wants to act on -- so this is a rendezvous, not just a search.
std::optional<std::string> WaitForPermissionRequest(SessionFixture& fixture,
                                                    std::chrono::milliseconds timeout = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        for (const auto& event : fixture.DrainQueue()) {
            if (event.has_permission_request()) return event.permission_request().request_id();
        }
        std::this_thread::sleep_for(5ms);
    }
    return std::nullopt;
}

int CountStreamEvents(const std::vector<v1::ServerEvent>& events) {
    int n = 0;
    for (const auto& event : events) {
        if (event.has_stream_event()) ++n;
    }
    return n;
}

}  // namespace

TEST_CASE("a session runs a full turn end to end", "[session]") {
    // The first time the agent loop, the tool executor, a real tool, the event
    // queue and the proto conversion all run together in one process. Every one
    // of those has been unit-tested through fakes; none of them had ever been
    // connected.
    SessionFixture fixture;
    auto tool = std::make_unique<testing::FakeTool>("read", tools::ToolCategory::ReadOnly,
                                                    "file contents");
    auto* tool_ptr = tool.get();
    fixture.tools_registry.Register(std::move(tool));

    fixture.Script(testing::ToolUseResponse("call-1", "read", {{"path", "x.cpp"}}),
                   {llm::TextDeltaEvent{0, "looking"}});
    fixture.Script(testing::TextResponse("here it is"));
    fixture.Start();

    fixture.session->SubmitTurn("read x.cpp");
    REQUIRE(fixture.session->WaitForIdleForTesting(5s));

    // Transcript: user -> assistant(tool_use) -> user(tool_result) -> assistant
    const auto& stored = fixture.store->messages;
    REQUIRE(stored.size() == 4);
    CHECK(stored[0].role == llm::Role::User);
    CHECK(stored[1].role == llm::Role::Assistant);
    REQUIRE(tool_ptr->calls.size() == 1);
    CHECK(tool_ptr->calls[0]["path"] == "x.cpp");

    const auto events = fixture.DrainQueue();
    CHECK(CountStreamEvents(events) >= 1);
}

TEST_CASE("tool execution is bracketed by lifecycle events", "[session]") {
    // A client that showed a tool as started must always see it finish, or its
    // running-tools panel accumulates entries that never clear.
    SessionFixture fixture;
    fixture.tools_registry.Register(
        std::make_unique<testing::FakeTool>("read", tools::ToolCategory::ReadOnly, "ok"));
    fixture.Script(testing::ToolUseResponse("call-1", "read", {{"path", "x.cpp"}}));
    fixture.Script(testing::TextResponse("done"));
    fixture.Start();

    fixture.session->SubmitTurn("go");
    REQUIRE(fixture.session->WaitForIdleForTesting(5s));

    std::vector<bool> started_flags;
    for (const auto& event : fixture.DrainQueue()) {
        if (event.has_tool_execution()) {
            CHECK(event.tool_execution().tool_name() == "read");
            CHECK(event.tool_execution().tool_use_id() == "call-1");
            started_flags.push_back(event.tool_execution().started());
        }
    }
    REQUIRE(started_flags.size() == 2);
    CHECK(started_flags[0]);
    CHECK_FALSE(started_flags[1]);
}

TEST_CASE("a failing tool still reports finished", "[session]") {
    SessionFixture fixture;
    auto tool = std::make_unique<testing::FakeTool>("read", tools::ToolCategory::ReadOnly, "boom");
    tool->set_is_error(true);
    fixture.tools_registry.Register(std::move(tool));
    fixture.Script(testing::ToolUseResponse("call-1", "read"));
    fixture.Script(testing::TextResponse("recovered"));
    fixture.Start();

    fixture.session->SubmitTurn("go");
    REQUIRE(fixture.session->WaitForIdleForTesting(5s));

    bool saw_error_finish = false;
    for (const auto& event : fixture.DrainQueue()) {
        if (event.has_tool_execution() && !event.tool_execution().started()) {
            saw_error_finish = event.tool_execution().is_error();
        }
    }
    CHECK(saw_error_finish);
}

TEST_CASE("tool output reaches the queue as its own event", "[session]") {
    // Tool output is not part of the LLM stream and must not be folded into it:
    // it is lossy by design downstream, while the transcript's ToolResultBlock
    // is the authoritative record.
    SessionFixture fixture;
    fixture.tools_registry.Register(std::make_unique<testing::FakeTool>(
        "bash", tools::ToolCategory::Execute, "streamed output"));
    fixture.Script(testing::ToolUseResponse("call-1", "bash"));
    fixture.Script(testing::TextResponse("done"));
    fixture.Start();

    fixture.session->SubmitTurn("go");
    REQUIRE(fixture.session->WaitForIdleForTesting(5s));

    bool saw_output = false;
    for (const auto& event : fixture.DrainQueue()) {
        if (event.has_tool_output_delta()) {
            CHECK(event.tool_output_delta().chunk() == "streamed output");
            CHECK(event.tool_output_delta().tool_use_id() == "call-1");
            saw_output = true;
        }
    }
    CHECK(saw_output);
}

TEST_CASE("every event carries its session id", "[session]") {
    // One client multiplexes several sessions over one channel, so an event
    // without a session id is unroutable.
    SessionFixture fixture;
    fixture.tools_registry.Register(
        std::make_unique<testing::FakeTool>("read", tools::ToolCategory::ReadOnly, "ok"));
    fixture.Script(testing::ToolUseResponse("call-1", "read"));
    fixture.Script(testing::TextResponse("done", llm::StopReason::EndTurn));
    fixture.Start("sess-42");

    fixture.session->SubmitTurn("go");
    REQUIRE(fixture.session->WaitForIdleForTesting(5s));

    const auto events = fixture.DrainQueue();
    REQUIRE_FALSE(events.empty());
    for (const auto& event : events) {
        CHECK(event.session_id() == "sess-42");
    }
}

TEST_CASE("cancelling mid-turn stops the worker without killing the session", "[session]") {
    // Cancel ends the TURN. The session survives and accepts the next one --
    // which is what distinguishes it from Terminate.
    SessionFixture fixture;
    fixture.Script(testing::TextResponse("first"));
    fixture.Script(testing::TextResponse("second"));
    fixture.Start();

    fixture.session->Cancel();
    fixture.session->SubmitTurn("hello");
    REQUIRE(fixture.session->WaitForIdleForTesting(5s));
    CHECK_FALSE(fixture.session->terminated());

    fixture.session->SubmitTurn("again");
    REQUIRE(fixture.session->WaitForIdleForTesting(5s));
    CHECK_FALSE(fixture.session->terminated());
}

TEST_CASE("a cancelled turn does not deny-lock the next one", "[session]") {
    // THE INVARIANT: Session::Cancel ends a turn, Session::Terminate ends a
    // session, and PendingRequests has to distinguish them. It once did not --
    // one permanent latch served both -- so the first CancelTurn made every
    // later permission prompt return "no answer" instantly, which
    // WirePermissionChecker maps to Deny. The session went on reporting itself
    // alive and accepting turns while silently refusing every gated tool, with
    // no log line and nothing on the wire to explain it.
    //
    // Collapse CancelAll() and Shutdown() back into one flag and the second half
    // of this test fails: the prompt never reaches the queue.
    auto fixture = std::make_shared<SessionFixture>();
    fixture->preset.permission_mode = testing::AllowAllPolicy();
    fixture->preset.permission_mode.execute = tools::PermissionMode::Ask;
    auto tool = std::make_unique<testing::FakeTool>("bash", tools::ToolCategory::Execute, "ran");
    auto* tool_ptr = tool.get();
    fixture->tools_registry.Register(std::move(tool));

    fixture->Script(testing::ToolUseResponse("call-1", "bash"));  // turn 1: cancelled at the gate
    fixture->Script(testing::ToolUseResponse("call-2", "bash"));  // turn 2: answered at the gate
    fixture->Script(testing::TextResponse("done"));
    fixture->Start();

    // Turn 1 parks on the prompt, then the user cancels instead of answering.
    fixture->session->SubmitTurn("run something");
    REQUIRE(WaitForPermissionRequest(*fixture).has_value());
    fixture->session->Cancel();
    REQUIRE(fixture->session->WaitForIdleForTesting(5s));
    REQUIRE_FALSE(fixture->session->terminated());
    CHECK(tool_ptr->calls.empty());  // denied, as a cancelled turn should be

    // Turn 2 must still be able to ask -- and be answered.
    fixture->session->SubmitTurn("run something else");
    const auto request_id = WaitForPermissionRequest(*fixture);
    REQUIRE(request_id.has_value());

    v1::PermissionResponse answer;
    answer.set_request_id(*request_id);
    answer.set_decision(v1::PERMISSION_DECISION_ALLOW);
    REQUIRE(fixture->session->pending().Resolve(
        *request_id, rpc::PermissionAnswer{answer.SerializeAsString()}));

    REQUIRE(fixture->session->WaitForIdleForTesting(5s));
    CHECK(tool_ptr->calls.size() == 1);
}

TEST_CASE("terminating with a turn in flight joins cleanly", "[session]") {
    // Termination reuses the cancellation path rather than adding a second
    // mechanism. If it hangs, every session teardown leaks its worker.
    auto fixture = std::make_shared<SessionFixture>();
    fixture->tools_registry.Register(
        std::make_unique<testing::FakeTool>("read", tools::ToolCategory::ReadOnly, "ok"));
    for (int i = 0; i < 20; ++i) {
        fixture->Script(testing::ToolUseResponse("call-" + std::to_string(i), "read"));
    }
    fixture->Script(testing::TextResponse("done"));
    fixture->Start();

    fixture->session->SubmitTurn("go");
    std::this_thread::sleep_for(20ms);

    auto terminated = testing::RunInBackground([fixture] {
        fixture->session->Terminate();
        return true;
    });
    REQUIRE(terminated.WaitFor(5s));
    CHECK(fixture->session->terminated());
}

TEST_CASE("terminating a worker parked on a full queue returns instead of deadlocking",
          "[session][invariant]") {
    // THE INVARIANT: Push() blocks at the queue cap and only Close() or Pop()
    // wakes it, so Terminate() must close the queue BEFORE joining the worker.
    // It once joined first -- reachable whenever a client stalls or dies
    // mid-stream and the queue fills: the writer stops draining, the worker
    // parks in Push, and the join waits for a worker that waits for a close
    // that was scheduled after the join. Restore the old order and this test
    // deadlocks rather than fails.
    SessionFixture fixture;
    fixture.Script(testing::TextResponse("hi"), {llm::TextDeltaEvent{0, "a chatty response"}});
    fixture.Start();

    // Fill the queue to its cap; nothing will drain it -- the writer is the
    // test's problem here, and it is not draining.
    std::size_t pushed = 0;
    while (true) {
        v1::ServerEvent filler;
        filler.set_session_id("sess-1");
        if (!fixture.session->queue().TryPush(std::move(filler))) break;
        ++pushed;
    }
    REQUIRE(pushed > 0);  // sanity: the cap is real and reachable

    // The turn's first stream event has nowhere to go: the worker parks in Push.
    fixture.session->SubmitTurn("hello");
    std::this_thread::sleep_for(50ms);

    auto terminated = testing::RunInBackground([&fixture] {
        fixture.session->Terminate();
        return true;
    });
    REQUIRE(terminated.WaitFor(5s));
    CHECK(fixture.session->terminated());
}

TEST_CASE("a subagent terminated before its turn ran still fires the completion hook",
          "[session][invariant]") {
    // on_turn_finished is the promise a blocking parent waits on. The
    // initial-attach reaper terminates a never-attached child, and a terminate
    // can race SubmitTurn -- in both cases the seeded turn never runs, so
    // RunOneTurn never fires the hook and the ONLY firing left is Terminate's.
    // Without it, the parent's future.get() waits on a promise nothing will
    // ever set, and the blocking parent hangs behind a child that is long gone.
    SessionFixture fixture;
    std::optional<agent::TurnResult> observed;
    fixture.on_turn_finished = [&](const agent::TurnResult& result) { observed = result; };
    fixture.Script(testing::TextResponse("never consumed"));
    fixture.Start();

    // No SubmitTurn: the work queue is empty when the session ends.
    fixture.session->Terminate();

    REQUIRE(observed.has_value());
    CHECK(observed->stop_reason == llm::StopReason::Cancelled);
}

TEST_CASE("the work queue is bounded and refusals are answered on the wire", "[session]") {
    // One turn can run for minutes, so an unbounded work queue let a client
    // enqueue transcript-sized turns faster than the worker consumed them.
    // Beyond the bound the submit must be REFUSED -- and answered, not dropped:
    // a client whose turn silently vanished would simply wait forever.
    auto fixture = std::make_shared<SessionFixture>();
    fixture->preset.permission_mode = testing::AllowAllPolicy();
    fixture->preset.permission_mode.execute = tools::PermissionMode::Ask;
    fixture->tools_registry.Register(
        std::make_unique<testing::FakeTool>("bash", tools::ToolCategory::Execute, "ran"));
    fixture->Script(testing::ToolUseResponse("call-1", "bash"));  // parks on the prompt
    fixture->Start();

    fixture->session->SubmitTurn("go");
    REQUIRE(WaitForPermissionRequest(*fixture).has_value());  // worker parked; consumes nothing

    int accepted = 0;
    int refused = 0;
    for (int i = 0; i < 1000; ++i) {
        if (fixture->session->SubmitTurn("flood " + std::to_string(i))) {
            ++accepted;
        } else {
            ++refused;
        }
    }
    CHECK(accepted > 0);
    CHECK(refused > 0);
    CHECK(accepted + refused == 1000);

    // Every refusal produced exactly one ErrorEvent -- no silent refusals, and
    // no fabricated answers for the accepted ones.
    int refusal_events = 0;
    for (const auto& event : fixture->DrainQueue()) {
        if (event.has_error() &&
            event.error().message().find("backlog") != std::string::npos) {
            ++refusal_events;
        }
    }
    CHECK(refusal_events == refused);

    fixture->session->Cancel();  // unpark the worker from the prompt
    fixture->session->Terminate();
}

TEST_CASE("cancelling during retry backoff reports Cancelled, not Unknown", "[session]") {
    // CancelTurn landing inside a backoff sleep exits the retry loop without a
    // response, and that path once fell out with StopReason::Unknown. The
    // headless exit code derives from this value -- a user cancel must not
    // report "model error".
    SessionFixture fixture;
    fixture.loop_config.retry_base_delay = 250ms;  // equal-jitter floor is 125ms
    fixture.Start();
    fixture.provider->PushError(
        llm::LlmError(llm::LlmErrorCategory::Transport, "backend is down"));

    fixture.session->SubmitTurn("hello");
    // Deterministic rendezvous: once the failed call is recorded the worker is
    // provably inside SleepForBackoff, so the cancel below cannot land before
    // the turn started (which would make it a legal no-op) -- it must land in
    // the window this test exists to cover.
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (fixture.provider->call_count() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    REQUIRE(fixture.provider->call_count() == 1);
    fixture.session->Cancel();
    REQUIRE(fixture.session->WaitForIdleForTesting(5s));

    std::optional<v1::StopReason> reported;
    for (const auto& event : fixture.DrainQueue()) {
        if (event.has_turn_completed()) reported = event.turn_completed().stop_reason();
    }
    REQUIRE(reported.has_value());
    CHECK(*reported == v1::STOP_REASON_CANCELLED);
}

TEST_CASE("a non-standard exception in the turn path degrades instead of terminating",
          "[session][invariant]") {
    // RunOneTurn once caught LlmError and std::exception only -- a throw of any
    // other type escaped WorkerMain and std::terminate the whole server, taking
    // every other session with it. The seq read and the observer notification
    // after the turn are as fallible as the turn itself, so the guard covers
    // them too. This store throws int, which no std::exception catch can see.
    struct ThrowingStore : testing::InMemoryHistoryStore {
        std::int64_t last_appended_seq() const override { throw 42; }
    };

    SessionFixture fixture;
    auto provider = std::make_unique<testing::FakeLlmProvider>();
    provider->PushResponse(testing::TextResponse("hi"));
    auto store = std::make_unique<ThrowingStore>();

    agent::SessionParams params;
    params.session_id = "sess-throw";
    params.agent_preset = "test";
    params.project_root = fixture.project_root;

    agent::Session session(std::move(params), fixture.preset, std::move(provider),
                           std::move(store), fixture.tools_registry, fixture.loop_config,
                           fixture.observers, common::DefaultLogger());

    session.SubmitTurn("hello");
    REQUIRE(session.WaitForIdleForTesting(5s));
    CHECK_FALSE(session.terminated());  // one degraded turn, a live session

    bool saw_error = false;
    while (auto event = session.queue().TryPop()) {
        if (event->has_error()) saw_error = true;
    }
    CHECK(saw_error);
    session.Terminate();
}

TEST_CASE("RejectCommand answers the request id even when nothing ran", "[session]") {
    // A saturated command pool refuses to run a command whose client is already
    // waiting on a CommandResult. Silence there is a hang for the client, so
    // refusal must still produce an answer.
    SessionFixture fixture;
    fixture.Start();

    agent::QueuedCommand command;
    command.request_id = "cmd-7";
    command.name = "anything";
    fixture.session->RejectCommand(command, "refused for testing");

    const auto events = fixture.DrainQueue();
    REQUIRE(events.size() == 1);
    CHECK(events[0].has_command_result());
    CHECK(events[0].command_result().request_id() == "cmd-7");
    CHECK(events[0].command_result().status() == v1::COMMAND_STATUS_ERROR);
    CHECK(events[0].command_result().message() == "refused for testing");
    fixture.session->Terminate();
}


TEST_CASE("terminate is idempotent", "[session]") {
    SessionFixture fixture;
    fixture.Script(testing::TextResponse("hi"));
    fixture.Start();
    fixture.session->Terminate();
    fixture.session->Terminate();
    CHECK(fixture.session->terminated());
}

TEST_CASE("the queue closes on termination so a writer can finish", "[session]") {
    SessionFixture fixture;
    fixture.Script(testing::TextResponse("hi"));
    fixture.Start();
    fixture.session->SubmitTurn("hello");
    REQUIRE(fixture.session->WaitForIdleForTesting(5s));

    fixture.session->Terminate();
    CHECK(fixture.session->queue().closed());

    // Drains to exhaustion and then reports closed, rather than blocking.
    while (fixture.session->queue().Pop().has_value()) {
    }
    SUCCEED("writer drained to completion without blocking");
}

TEST_CASE("a turn submitted after termination is ignored", "[session]") {
    SessionFixture fixture;
    fixture.Script(testing::TextResponse("hi"));
    fixture.Start();
    fixture.session->Terminate();
    fixture.session->SubmitTurn("too late");
    CHECK(fixture.store->messages.empty());
}

TEST_CASE("a provider error surfaces as an ErrorEvent and the session survives",
          "[session]") {
    // One failed turn is not a reason to tear down a conversation the user can
    // retry -- but it must be visible, not silent.
    SessionFixture fixture;
    fixture.loop_config.max_retries = 0;
    fixture.Start();
    // No scripted steps: FakeLlmProvider throws a non-retryable RequestError.

    fixture.session->SubmitTurn("hello");
    REQUIRE(fixture.session->WaitForIdleForTesting(5s));

    bool saw_error = false;
    for (const auto& event : fixture.DrainQueue()) {
        if (event.has_error()) {
            CHECK_FALSE(event.error().message().empty());
            saw_error = true;
        }
    }
    CHECK(saw_error);
    CHECK_FALSE(fixture.session->terminated());
}

TEST_CASE("observers see the session's whole life", "[session]") {
    struct Recorder : agent::SessionObserver {
        void OnSessionCreated(const agent::SessionInfo& info) override {
            created = info.session_id;
        }
        void OnSessionEnded(const agent::SessionInfo& info) override { ended = info.session_id; }
        void OnTurnCompleted(const agent::SessionInfo&, const agent::TurnSummary& s) override {
            ++turns;
            stop_reason = s.stop_reason;
        }
        std::string created, ended;
        int turns = 0;
        llm::StopReason stop_reason = llm::StopReason::Unknown;
    };

    auto recorder = std::make_shared<Recorder>();
    {
        SessionFixture fixture;
        fixture.observers.Register(recorder);
        fixture.Script(testing::TextResponse("hi"));
        fixture.Start("sess-obs");

        fixture.session->SubmitTurn("hello");
        REQUIRE(fixture.session->WaitForIdleForTesting(5s));
        fixture.session->Terminate();
        fixture.observers.WaitForIdleForTesting();
    }
    CHECK(recorder->created == "sess-obs");
    CHECK(recorder->ended == "sess-obs");
    CHECK(recorder->turns == 1);
    CHECK(recorder->stop_reason == llm::StopReason::EndTurn);
}

TEST_CASE("observers see a FAILED turn end too", "[session][invariant]") {
    // The wire's TurnCompletedEvent fires on every exit path because a client
    // that learns a turn ended only by seeing it succeed hangs on every error.
    // OnTurnCompleted is the same obligation in-process: an observer holding
    // per-turn state has no other end-of-turn signal, so an early return from
    // RunOneTurn's catch blocks leaked it once per failed turn -- and a failing
    // provider is exactly when an observer most wants to know.
    struct Recorder : agent::SessionObserver {
        void OnTurnCompleted(const agent::SessionInfo&, const agent::TurnSummary& s) override {
            ++turns;
            stop_reason = s.stop_reason;
        }
        int turns = 0;
        llm::StopReason stop_reason = llm::StopReason::EndTurn;
    };

    auto recorder = std::make_shared<Recorder>();
    {
        SessionFixture fixture;
        fixture.observers.Register(recorder);
        // Nothing scripted: the fake provider throws, which is what a provider
        // that is down looks like from here.
        fixture.Start("sess-fail");

        fixture.session->SubmitTurn("hello");
        REQUIRE(fixture.session->WaitForIdleForTesting(5s));
        fixture.session->Terminate();
        fixture.observers.WaitForIdleForTesting();
    }
    CHECK(recorder->turns == 1);
    CHECK(recorder->stop_reason == llm::StopReason::Unknown);
}

TEST_CASE("a client-requested run mode overrides the preset's", "[session]") {
    // AllowOnly is a property of the CLIENT, not the config: a headless run has
    // no human to answer a prompt. With the override in force an Ask-gated tool
    // must auto-deny rather than block forever -- so this test completing at all
    // is the assertion.
    SessionFixture fixture;
    fixture.preset.permission_mode.execute = tools::PermissionMode::Ask;
    fixture.preset.run_mode = tools::PermissionRunMode::Normal;
    fixture.tools_registry.Register(
        std::make_unique<testing::FakeTool>("bash", tools::ToolCategory::Execute, "ran"));
    fixture.Script(testing::ToolUseResponse("call-1", "bash"));
    fixture.Script(testing::TextResponse("done"));

    auto provider = std::make_unique<testing::FakeLlmProvider>();
    provider->PushResponse(testing::ToolUseResponse("call-1", "bash"));
    provider->PushResponse(testing::TextResponse("done"));

    agent::SessionParams params;
    params.session_id = "sess-headless";
    params.agent_preset = "test";
    params.project_root = fixture.project_root;
    params.run_mode_override = tools::PermissionRunMode::AllowOnly;

    auto owned_store = std::make_unique<testing::InMemoryHistoryStore>();
    auto* store = owned_store.get();
    agent::Session session(params, fixture.preset, std::move(provider), std::move(owned_store),
                           fixture.tools_registry, fixture.loop_config, fixture.observers,
                           common::DefaultLogger());
    session.SubmitTurn("run something");
    REQUIRE(session.WaitForIdleForTesting(5s));  // did not block on a prompt
    session.Terminate();

    // The tool was refused, and the refusal is in the transcript as a result
    // block rather than as a missing turn.
    CHECK(store->messages.size() == 4);
}

TEST_CASE("two sessions do not share bash state", "[session]") {
    // The real bash tool, forking real processes, keyed by session id. This is
    // the integration the per-session state map exists for -- and the one that
    // would silently regress into a single global shell.
    testing::TempDir tempdir;
    const auto base = tempdir.path();
    std::filesystem::create_directories(base / "one");
    std::filesystem::create_directories(base / "two");

    tools_bash::BashSessionStateStore store;

    auto run = [&](const std::string& session_id, const std::filesystem::path& root,
                   const std::string& command) {
        SessionFixture fixture(root);
        fixture.tools_registry.Register(
            tools_bash::MakeBashTool(tools_bash::ParseBashSettings({}), store));
        fixture.Script(testing::ToolUseResponse("c1", "bash", {{"command", command}}));
        fixture.Script(testing::TextResponse("done"));
        fixture.Start(session_id);
        fixture.session->SubmitTurn("go");
        REQUIRE(fixture.session->WaitForIdleForTesting(20s));
        fixture.session->Terminate();
    };

    run("sess-a", base / "one", "pwd");
    run("sess-b", base / "two", "pwd");

    CHECK(store.Has("sess-a"));
    CHECK(store.Has("sess-b"));
    CHECK(store.size() == 2);
}

TEST_CASE("ending a session releases only that session's bash state", "[session]") {
    // The leak OnSessionEnded exists to close. Erasing too much would be just as
    // wrong as erasing nothing: a live session would silently lose its cwd.
    tools_bash::BashSessionStateStore store;
    store.Snapshot("sess-a", std::filesystem::temp_directory_path());
    store.Snapshot("sess-b", std::filesystem::temp_directory_path());
    REQUIRE(store.size() == 2);

    struct BashObserver : agent::SessionObserver {
        explicit BashObserver(tools_bash::BashSessionStateStore& s) : store(s) {}
        void OnSessionEnded(const agent::SessionInfo& info) override {
            store.Erase(info.session_id);
        }
        tools_bash::BashSessionStateStore& store;
    };

    agent::SessionObserverRegistry observers(common::DefaultLogger());
    observers.Register(std::make_shared<BashObserver>(store));

    agent::SessionInfo info;
    info.session_id = "sess-a";
    observers.NotifySessionEnded(info);
    observers.WaitForIdleForTesting();

    CHECK_FALSE(store.Has("sess-a"));
    CHECK(store.Has("sess-b"));
    CHECK(store.size() == 1);
}

// --- subagent plumbing --------------------------------------------------------
//
// A subagent is the same Session entity, distinguished by one field plus
// restrictions derived from it. These tests cover the Session half of that
// wiring; the spawn itself is tested in subagent_spawn_test.cpp.

TEST_CASE("a session fires its turn-finished hook with the final text",
          "[session][subagent]") {
    // The hook is how the spawner captures a subagent's result -- and what a
    // blocking parent is ultimately waiting on.
    SessionFixture fixture;
    agent::TurnResult captured;
    bool fired = false;
    fixture.on_turn_finished = [&](const agent::TurnResult& result) {
        captured = result;
        fired = true;
    };
    fixture.Script(testing::TextResponse("child result"));
    fixture.Start();

    fixture.session->SubmitTurn("do the task");
    REQUIRE(fixture.session->WaitForIdleForTesting(5s));

    CHECK(fired);
    CHECK(captured.final_text == "child result");
    CHECK(captured.stop_reason == llm::StopReason::EndTurn);
    CHECK_FALSE(captured.hit_max_turns);
}

TEST_CASE("the turn-finished hook fires on a FAILED turn too", "[session][subagent]") {
    // A blocking parent waiting on a child whose provider died must unblock with
    // an error, not hang. Default-constructed TurnResult is the failure value.
    SessionFixture fixture;
    fixture.loop_config.max_retries = 0;
    agent::TurnResult captured;
    bool fired = false;
    fixture.on_turn_finished = [&](const agent::TurnResult& result) {
        captured = result;
        fired = true;
    };
    fixture.Start();
    // Nothing scripted: the fake provider throws a non-retryable RequestError.

    fixture.session->SubmitTurn("do the task");
    REQUIRE(fixture.session->WaitForIdleForTesting(5s));

    CHECK(fired);
    CHECK(captured.final_text.empty());
    CHECK(captured.stop_reason == llm::StopReason::Unknown);
}

TEST_CASE("a finished subagent session ignores further turns", "[session][subagent]") {
    // A subagent's life is one seeded turn. Once the completion hook has fired,
    // a stray client turn would fire it a second time -- so it must not run.
    SessionFixture fixture;
    int hook_calls = 0;
    fixture.on_turn_finished = [&](const agent::TurnResult&) { ++hook_calls; };
    fixture.Script(testing::TextResponse("done"));
    fixture.Start();

    fixture.session->SubmitTurn("the task");
    REQUIRE(fixture.session->WaitForIdleForTesting(5s));

    fixture.session->SubmitTurn("one more thing");
    fixture.session->WaitForIdleForTesting(5s);

    CHECK(hook_calls == 1);
    CHECK(fixture.provider->call_count() == 1);
    CHECK(fixture.store->messages.size() == 2);  // only the first turn's pair
}

TEST_CASE("seeded grants reach the session's executor", "[session][subagent]") {
    // Copy-at-spawn: a child holding the parent's grants runs an already-approved
    // invocation without prompting. The prompt would show up on the queue and
    // block the turn -- so this test completing at all is part of the assertion.
    SessionFixture fixture;
    fixture.preset.permission_mode.execute = tools::PermissionMode::Ask;
    auto tool = std::make_unique<testing::FakeTool>("bash", tools::ToolCategory::Execute, "ran");
    tools::PermissionDisclosure disclosure;
    disclosure.summary = "git status";
    disclosure.grants.push_back(tools::GrantOffer{"bash:/usr/bin/git", "/usr/bin/git *"});
    tool->set_disclosure(std::move(disclosure));
    fixture.tools_registry.Register(std::move(tool));
    fixture.seed_grants = {"bash:/usr/bin/git"};
    fixture.Script(testing::ToolUseResponse("call-1", "bash"));
    fixture.Script(testing::TextResponse("done"));
    fixture.Start();

    fixture.session->SubmitTurn("run it");
    REQUIRE(fixture.session->WaitForIdleForTesting(5s));

    for (const auto& event : fixture.DrainQueue()) {
        CHECK_FALSE(event.has_permission_request());  // never asked
    }
}

TEST_CASE("a policy ceiling reaches the session's executor", "[session][subagent]") {
    // The child preset allows execute outright; the parent's Ask policy is the
    // ceiling. Under AllowOnly the Ask auto-denies instead of blocking, which is
    // what makes the denial observable without answering a prompt.
    SessionFixture fixture;
    fixture.preset.permission_mode = testing::AllowAllPolicy();
    fixture.preset.run_mode = tools::PermissionRunMode::Normal;
    auto tool = std::make_unique<testing::FakeTool>("bash", tools::ToolCategory::Execute, "ran");
    auto* tool_ptr = tool.get();
    fixture.tools_registry.Register(std::move(tool));
    tools::PermissionPolicy ceiling = testing::AllowAllPolicy();
    ceiling.execute = tools::PermissionMode::Ask;
    fixture.policy_ceiling = ceiling;
    fixture.Script(testing::ToolUseResponse("call-1", "bash"));
    fixture.Script(testing::TextResponse("done"));

    auto provider = std::make_unique<testing::FakeLlmProvider>();
    provider->PushResponse(testing::ToolUseResponse("call-1", "bash"));
    provider->PushResponse(testing::TextResponse("done"));

    agent::SessionParams params;
    params.session_id = "sess-child";
    params.agent_preset = "test";
    params.project_root = fixture.project_root;
    params.policy_ceiling = ceiling;
    params.run_mode_override = tools::PermissionRunMode::AllowOnly;

    auto owned_store = std::make_unique<testing::InMemoryHistoryStore>();
    auto* store = owned_store.get();
    agent::Session session(params, fixture.preset, std::move(provider), std::move(owned_store),
                           fixture.tools_registry, fixture.loop_config, fixture.observers,
                           common::DefaultLogger());
    session.SubmitTurn("run something");
    REQUIRE(session.WaitForIdleForTesting(5s));  // did not block on a prompt
    session.Terminate();

    CHECK(tool_ptr->calls.empty());  // the ceiling forced Ask; AllowOnly denied it
    CHECK(store->messages.size() == 4);
}

TEST_CASE("a queued injected message is appended at a turn boundary", "[session][subagent]") {
    // The non-blocking completion path: a child's worker queues its result while
    // the parent is between turns, and it must land in the transcript whole --
    // never between a tool_use and its tool_result.
    SessionFixture fixture;
    fixture.Script(testing::TextResponse("next answer"));
    fixture.Start();

    fixture.session->QueueInjectedMessage(
        llm::Message{llm::Role::User,
                     {llm::ToolResultBlock{"spawn-1", {llm::TextBlock{"child result"}}, false}}});
    fixture.session->SubmitTurn("next turn");
    REQUIRE(fixture.session->WaitForIdleForTesting(5s));

    // Order: injected seed of turn 1 never existed; this turn appended its seed
    // and response FIRST, then the boundary flush appended the injection.
    const auto& stored = fixture.store->messages;
    REQUIRE(stored.size() == 3);
    CHECK(stored[0].role == llm::Role::User);      // "next turn"
    CHECK(stored[1].role == llm::Role::Assistant);  // "next answer"
    REQUIRE(stored[2].role == llm::Role::User);
    REQUIRE(stored[2].content.size() == 1);
    CHECK(std::get<llm::ToolResultBlock>(stored[2].content.front()).tool_use_id == "spawn-1");
}
