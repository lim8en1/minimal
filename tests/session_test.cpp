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

        agent::SessionParams params;
        params.session_id = std::move(session_id);
        params.name = "test session";
        params.agent_preset = "test";
        params.project_root = project_root;

        session = std::make_unique<agent::Session>(std::move(params), preset,
                                                    std::move(owned_provider), tools_registry,
                                                    loop_config, observers,
                                                    common::DefaultLogger());
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
    testing::FakeLlmProvider* provider = nullptr;
    std::unique_ptr<agent::Session> session;
};

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
    const auto stored = fixture.session->history().Read(0, 0);
    REQUIRE(stored.size() == 4);
    CHECK(stored[3].seq == 0);  // descending by seq
    CHECK(stored[0].seq == 3);
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
    CHECK(fixture.session->history().size() == 0);
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

    agent::Session session(params, fixture.preset, std::move(provider), fixture.tools_registry,
                           fixture.loop_config, fixture.observers, common::DefaultLogger());
    session.SubmitTurn("run something");
    REQUIRE(session.WaitForIdleForTesting(5s));  // did not block on a prompt
    session.Terminate();

    // The tool was refused, and the refusal is in the transcript as a result
    // block rather than as a missing turn.
    CHECK(session.history().size() == 4);
}

TEST_CASE("two sessions do not share bash state", "[session]") {
    // The real bash tool, forking real processes, keyed by session id. This is
    // the integration the per-session state map exists for -- and the one that
    // would silently regress into a single global shell.
    const auto base = std::filesystem::temp_directory_path() / "minimal_session_bash_test";
    std::filesystem::remove_all(base);
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

    std::filesystem::remove_all(base);
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
