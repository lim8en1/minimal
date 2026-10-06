#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "deadline.hpp"
#include "fakes.hpp"
#include "session_events.hpp"
#include "minimal/agent/agent_preset_registry.hpp"
#include "minimal/agent/provider_resolver.hpp"
#include "minimal/agent/session.hpp"
#include "minimal/rpc/stream_attachment.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/config/settings_config.hpp"
#include "minimal/rpc/event_translator.hpp"
#include "minimal/rpc/session_wiring.hpp"
#include "minimal/rpc/transport_channels.hpp"
#include "minimal/tools/permission_advisor.hpp"
#include "minimal/mcp/mcp_permission_model.hpp"
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
        params.permission_checker = std::move(injected_checker);
        params.advisor_cancel_source = injected_cancel_source;
        params.transport = injected_transport;
        params.seed_grants = seed_grants;
        params.policy_ceiling = policy_ceiling;
        params.on_turn_finished = on_turn_finished;
        params.wake_on_injection = wake_on_injection;
        params.pause_auto_resume_on_submit = pause_auto_resume_on_submit;
        // A preset registry turns the per-turn /agent borrow path on; tests
        // that only run plain turns leave it null and never touch it.
        params.environment.presets = preset_registry;
        params.environment.provider_resolver = provider_resolver;
        // Builds whatever the test did not inject above: a real transport
        // (unless one already came from rpc::ApplyPermissionAdvisor), the
        // default WirePermissionChecker (unless a fake/advised one was
        // injected), and always the WireConfirmationBroker -- the same
        // funnel SessionRegistry::CreateResolved runs for every production
        // session.
        rpc::FinishSessionWiring(params);

        session = std::make_shared<agent::Session>(
            std::move(params), preset, std::move(owned_provider), std::move(owned_store),
            tools_registry, loop_config, observers, common::DefaultLogger());
        stream = std::make_unique<testing::SessionStream>(session);
    }

    void Script(llm::ChatResponse response, std::vector<llm::StreamEvent> events = {}) {
        scripted.emplace_back(std::move(response), std::move(events));
    }

    // Injected through SessionParams::permission_checker -- the seam an advised
    // (or test) checker crosses. Null keeps the wire default.
    std::unique_ptr<tools::PermissionChecker> injected_checker;
    // Set alongside an injected AdvisedPermissionChecker: the Session keeps
    // this holder's flag and fires it in Cancel()/Terminate().
    std::shared_ptr<tools::AdvisorCancelSource> injected_cancel_source;
    // A transport the test built itself -- the one an advised composition
    // (FinishSessionWiring with selected_advisor) already made, so checker and
    // session publish through the SAME transport, or a decorated one. Null
    // keeps FinishSessionWiring's own default (a fresh transport).
    std::shared_ptr<agent::SessionTransport> injected_transport;

    // Drains everything currently queued for the fixture's subscriber. The
    // writer thread's job, done inline.
    std::vector<v1::ServerEvent> DrainQueue() { return stream->Drain(); }

    ~SessionFixture() {
        // The stream unsubscribes first (declared after session, destroyed
        // before it), then Terminate closes whatever remains.
        stream.reset();
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
    // Optional preset registry for the per-turn borrow path; must outlive the
    // session (declare it in the test, before Start()).
    const agent::AgentPresetRegistry* preset_registry = nullptr;
    // Optional resolver for the provider-switch paths (/model, /agent,
    // per-turn overrides); must outlive the session (declare in the test,
    // before Start()).
    agent::ProviderResolver* provider_resolver = nullptr;
    // The idle-wake setting: an out-of-band injection on an IDLE session runs a
    // seedless turn (the injected messages are the user content).
    bool wake_on_injection = false;
    // The auto-resume setting: a turn submitted while paused clears the flag.
    // Off in the hold tests so a submission is provably held.
    bool pause_auto_resume_on_submit = true;
    testing::FakeLlmProvider* provider = nullptr;
    testing::InMemoryHistoryStore* store = nullptr;  // owned by the session
    std::shared_ptr<agent::Session> session;
    // Declared AFTER session so it is destroyed first and unsubscribes before
    // the session dies. The stream also OWNS a shared_ptr to the session, so
    // a test that resets fixture.session mid-case cannot dangle the stream.
    std::unique_ptr<testing::SessionStream> stream;
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

bool WaitForCallCount(testing::FakeLlmProvider& provider, std::size_t expected,
                      std::chrono::milliseconds timeout = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (provider.call_count() < expected && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    return provider.call_count() == expected;
}

}  // namespace


TEST_CASE("a checker injected through SessionParams is honored end to end",
          "[session][advisor]") {
    // The seam the permission advisor composes through: the params producer can
    // hand the session any PermissionChecker, and the session uses it INSTEAD of
    // the wire default. Making the session's checker concrete again fails this
    // test -- the fake would have nowhere to arrive.
    SessionFixture fixture;
    auto tool = std::make_unique<testing::FakeTool>("bash", tools::ToolCategory::Execute, "ran");
    auto* tool_ptr = tool.get();
    fixture.tools_registry.Register(std::move(tool));
    fixture.preset.permission_mode.execute = tools::PermissionMode::Ask;

    auto checker = std::make_unique<testing::AlwaysDenyPermissionChecker>();
    auto* checker_ptr = checker.get();
    fixture.injected_checker = std::move(checker);

    fixture.Script(testing::ToolUseResponse("call-1", "bash", {{"command", "ls"}}));
    fixture.Script(testing::TextResponse("I could not run that"));
    fixture.Start();

    fixture.session->SubmitTurn("run it");
    REQUIRE(fixture.session->WaitForIdleForTesting(5s));

    REQUIRE(checker_ptr->seen.size() == 1);  // the injected checker decided, not the wire
    CHECK(checker_ptr->seen.front().tool_name == "bash");
    CHECK(tool_ptr->calls.empty());          // deny gates before execution
}

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

namespace {

// Records what a tool sees on its ToolContext: the deferred-completion channel a
// background task (bash run_in_background, python) schedules its result through.
class InjectionProbeTool : public tools::Tool {
public:
    InjectionProbeTool() {
        spec_.name = "probe";
        spec_.category = tools::ToolCategory::ReadOnly;
        spec_.definition.name = "probe";
        spec_.definition.description = "injection probe";
        spec_.definition.input_schema = nlohmann::json::object();
    }
    const tools::ToolSpec& spec() const override { return spec_; }
    tools::ToolResult execute(const nlohmann::json&, tools::ToolContext& ctx) override {
        had_channel = static_cast<bool>(ctx.schedule_injection);
        channel = ctx.schedule_injection;  // a background task keeps this past the call
        lifetime = ctx.session_lifetime;
        output = ctx.emit_output;
        if (ctx.schedule_injection) {
            delivered = ctx.schedule_injection(
                llm::Message{llm::Role::User, {llm::TextBlock{"background result"}}});
        }
        return tools::ToolResult{false, "ok", {}};
    }
    bool had_channel = false;
    bool delivered = false;
    std::function<bool(llm::Message)> channel;
    std::weak_ptr<const void> lifetime;
    std::function<void(std::string_view)> output;

private:
    tools::ToolSpec spec_;
};

}  // namespace

TEST_CASE("a session gives its tools a working deferred-completion channel", "[session]") {
    // Background bash / python refuse to start without ToolContext::schedule_injection
    // ("no deferred-completion channel"), and nothing below the Session wiring tests
    // it: the loop copies SessionContext::injection_sink onto every context, so the
    // Session must set it.
    SessionFixture fixture;
    auto tool = std::make_unique<InjectionProbeTool>();
    auto* probe = tool.get();
    fixture.tools_registry.Register(std::move(tool));
    fixture.Script(testing::ToolUseResponse("call-1", "probe", nlohmann::json::object()));
    fixture.Script(testing::TextResponse("done"));
    fixture.Start();

    fixture.session->SubmitTurn("probe it");
    REQUIRE(fixture.session->WaitForIdleForTesting(5s));

    REQUIRE(probe->had_channel);
    CHECK(probe->delivered);
    // The injected result is flushed into the transcript at a turn boundary.
    bool found = false;
    for (const auto& stored : fixture.store->messages) {
        if (llm::TextOf(stored.content) == "background result") found = true;
    }
    CHECK(found);
}

TEST_CASE("session_lifetime expires at Terminate even while a tool holds its sinks",
          "[session]") {
    // A background watcher keeps schedule_injection (and the task-event and
    // output sinks) past the call. Those hold the session's channels block, so
    // session_lifetime -- the watcher's ONLY death signal -- must not be tied
    // to it, or background work is never killed when the session ends.
    SessionFixture fixture;
    auto tool = std::make_unique<InjectionProbeTool>();
    auto* probe = tool.get();
    fixture.tools_registry.Register(std::move(tool));
    fixture.Script(testing::ToolUseResponse("call-1", "probe", nlohmann::json::object()));
    fixture.Script(testing::TextResponse("done"));
    fixture.Start();
    fixture.session->SubmitTurn("probe it");
    REQUIRE(fixture.session->WaitForIdleForTesting(5s));

    REQUIRE(probe->channel);
    CHECK_FALSE(probe->lifetime.expired());
    fixture.session->Terminate();
    CHECK(probe->lifetime.expired());
    CHECK_FALSE(probe->channel(llm::Message{llm::Role::User, {llm::TextBlock{"late"}}}));
    // The output sink outlives the session too (it must not reach into the
    // destroyed loop): calling it after the session is gone is safe.
    fixture.session.reset();
    if (probe->output) probe->output("late output");
}

TEST_CASE("a stream attachment pairs subscribe/notify with unsubscribe/notify",
          "[session][stream_attachment]") {
    // The fixture already holds one subscriber (its own queue reader), so counts are
    // relative to that baseline.
    SessionFixture fixture;
    fixture.Start();
    agent::Session& session = *fixture.session;

    rpc::StreamAttachment first(session);
    const std::size_t baseline = first.SubscriberCount();
    CHECK(session.ever_attached());
    {
        rpc::StreamAttachment second(session);
        CHECK(first.SubscriberCount() == baseline + 1);
    }  // destruction detaches exactly this stream
    CHECK(first.SubscriberCount() == baseline);
    CHECK(session.has_subscribers());

    first.Detach();
    first.Detach();  // idempotent: no second Unsubscribe / NotifyDetached to underflow a count
    rpc::StreamAttachment third(session);
    CHECK(third.SubscriberCount() == baseline);  // `first` really left, once
}

TEST_CASE("a stream attachment delivers what is published to it", "[session][stream_attachment]") {
    SessionFixture fixture;
    fixture.Start();
    rpc::StreamAttachment attachment(*fixture.session);

    v1::ServerEvent event;
    event.set_session_id(fixture.session->info().session_id);
    event.mutable_session_ended();
    REQUIRE(attachment.PublishTo(event));
    // A later subscriber is told it missed earlier events (resync) before anything
    // targeted at it; skip that notice and find ours.
    bool found = false;
    for (int i = 0; i < 4 && !found; ++i) {
        const auto popped = attachment.Pop();
        REQUIRE(popped.has_value());
        found = popped->has_session_ended();
    }
    CHECK(found);

    attachment.Detach();
    CHECK_FALSE(attachment.PublishTo(event));  // the stream's queue is closed
}

TEST_CASE("a background result landing on an idle session wakes it (wake_on_injection)",
          "[session]") {
    SessionFixture fixture;
    fixture.wake_on_injection = true;
    auto tool = std::make_unique<InjectionProbeTool>();
    auto* probe = tool.get();
    fixture.tools_registry.Register(std::move(tool));
    fixture.Script(testing::ToolUseResponse("call-1", "probe", nlohmann::json::object()));
    fixture.Script(testing::TextResponse("first turn done"));
    fixture.Script(testing::TextResponse("woken by the background result"));
    fixture.Start();

    fixture.session->SubmitTurn("start a task");
    REQUIRE(fixture.session->WaitForIdleForTesting(5s));
    REQUIRE(probe->channel);
    const std::size_t calls_before = fixture.provider->call_count();

    // The watcher thread of a background task finishing after the turn ended.
    llm::ToolResultBlock result;
    result.tool_use_id = "bg-1";
    result.content.push_back(llm::TextBlock{"task finished"});
    std::thread watcher([&] {
        probe->channel(llm::Message{llm::Role::User, {std::move(result)}});
    });
    watcher.join();

    // The sink woke the worker: a seedless turn ran and the provider was asked again.
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (fixture.provider->call_count() == calls_before &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    CHECK(fixture.provider->call_count() > calls_before);
    REQUIRE(fixture.session->WaitForIdleForTesting(5s));
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

    REQUIRE(testing::Transport(*fixture->session)
                .pending()
                .Resolve(*request_id, rpc::PermissionAnswer{tools::PermissionDecision::Allow, {}}));

    REQUIRE(fixture->session->WaitForIdleForTesting(5s));
    CHECK(tool_ptr->calls.size() == 1);
}

TEST_CASE("an advise thread abandoned at the budget polls a flag that outlives the session",
          "[session][advisor][invariant]") {
    // THE lifetime hazard AdvisorCancelSource's shared flag exists for: at
    // budget expiry the advise thread is DETACHED and keeps polling
    // is_cancelled() -- here, potentially past the point where this test
    // terminates and destroys the Session. An earlier draft installed the
    // predicate as `[this] { return IsCancelled(); }` on the Session: the
    // shared_ptr kept the HOLDER alive, not the Session it pointed at, so
    // every post-teardown poll was a use-after-free (the ASan preset fails
    // the run; a plain build reads garbage). The wait below joins the
    // abandoned thread's final poll by observation -- it must have read the
    // flag Terminate set, from memory nothing else owns any more.
    using namespace std::chrono_literals;

    struct PollsLate : tools::PermissionAdvisor {
        std::optional<tools::PermissionResponse> Advise(
            const tools::PermissionRequest&, const tools::AdvisorContext& ctx) override {
            std::this_thread::sleep_for(500ms);  // the budget (100ms) expires first
            observed.store(ctx.is_cancelled(), std::memory_order_release);
            polls.fetch_add(1, std::memory_order_release);
            return std::nullopt;  // moot: the answer was abandoned at the budget
        }
        std::atomic<bool> observed{false};
        std::atomic<int> polls{0};
    };

    SessionFixture fixture;
    fixture.preset.permission_mode.execute = tools::PermissionMode::Ask;
    fixture.tools_registry.Register(
        std::make_unique<testing::FakeTool>("slow", tools::ToolCategory::Execute));
    auto advisor = std::make_shared<PollsLate>();
    // The producer composition (selected_advisor + FinishSessionWiring): the
    // inner checker is the REAL wire checker over a real transport, so the
    // budget's fall-through is the human path and the prompt actually
    // round-trips.
    {
        agent::SessionParams temp;
        temp.session_id = "sess-advise";
        temp.selected_advisor = advisor;
        temp.advisor_timeout = std::chrono::milliseconds{100};
        rpc::FinishSessionWiring(temp);
        fixture.injected_checker = std::move(temp.permission_checker);
        fixture.injected_cancel_source = temp.advisor_cancel_source;
        fixture.injected_transport = temp.transport;
    }
    fixture.Script(testing::ToolUseResponse("tu1", "slow"));
    fixture.Script(testing::TextResponse("unreached"));
    fixture.Start("sess-advise");
    REQUIRE(fixture.session->SubmitTurn("go"));
    const auto request_id = WaitForPermissionRequest(fixture);
    REQUIRE(request_id.has_value());  // the budget expired; the human decides
    REQUIRE(testing::Transport(*fixture.session)
                .pending()
                .Resolve(*request_id, rpc::PermissionAnswer{tools::PermissionDecision::Allow, {}}));
    REQUIRE(fixture.session->WaitForIdleForTesting(5s));

    // Terminate fires the flag; destroying the session drops every owner but
    // the detached thread's own copies. The advisor may still be asleep.
    fixture.session->Terminate();
    fixture.session.reset();

    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (advisor->polls.load() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    CHECK(advisor->polls.load() == 1);
    CHECK(advisor->observed.load());
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

TEST_CASE("terminating an idle session returns instead of deadlocking on a lost wakeup",
          "[session][invariant]") {
    // THE INVARIANT: Terminate() must wake a worker that is ALREADY PARKED on
    // turn_cv_ with an empty queue -- the ordinary idle state between turns,
    // not a turn in flight (the prior test) or a wedged subscriber queue
    // (the next one). Terminate mutates terminated_ (via exchange, outside
    // mutex_) and then notifies; if the worker evaluates the wait predicate
    // (reading terminated_ == false) in the window between that exchange and
    // the notify, it parks having already missed the wakeup and never wakes.
    // Sleeping past WaitForIdleForTesting before calling Terminate maximizes
    // the chance the worker is genuinely parked in cv.wait when Terminate
    // runs, rather than still unwinding its own turn-boundary bookkeeping.
    SessionFixture fixture;
    fixture.Script(testing::TextResponse("hi"));
    fixture.Start();

    fixture.session->SubmitTurn("hello");
    REQUIRE(fixture.session->WaitForIdleForTesting(5s));
    std::this_thread::sleep_for(20ms);

    auto terminated = testing::RunInBackground([&fixture] {
        fixture.session->Terminate();
        return true;
    });
    REQUIRE(terminated.WaitFor(5s));
    CHECK(fixture.session->terminated());
}

TEST_CASE("Session::Terminate survives a throwing event sink without aborting",
          "[session][invariant]") {
    // Terminate() is called from ~Session() (implicitly noexcept), from
    // SessionRegistry::ReaperMain's thread body, and from TerminateAll() --
    // none of which has a handler, so an uncaught throw ANYWHERE inside
    // Terminate() is std::terminate on an ordinary session-lifetime path
    // (a wedged event sink, an allocation failure), not an exotic one.
    // Wraps the real wire event sink so it throws on the terminal
    // SessionEnded event -- the exact call Terminate's own body used to
    // leave unguarded -- and confirms Terminate() still returns and joins
    // cleanly (if it did not, the whole test process would abort here, not
    // just this assertion fail).
    SessionFixture fixture;
    fixture.Script(testing::TextResponse("hi"));

    // The real wire transport, except that publishing SessionEnded throws.
    class ThrowOnEnd : public rpc::EventTranslator {
    public:
        using EventTranslator::EventTranslator;
        bool Publish(const agent::SessionEvent& event) override {
            if (std::holds_alternative<agent::SessionEnded>(event)) {
                throw std::runtime_error("event sink exploded on SessionEnded");
            }
            return EventTranslator::Publish(event);
        }
    };
    fixture.injected_transport = std::make_shared<ThrowOnEnd>(
        "sess-throw-terminate", rpc::MakeTransportChannels("sess-throw-terminate"));
    fixture.Start("sess-throw-terminate");

    fixture.session->SubmitTurn("hello");
    REQUIRE(fixture.session->WaitForIdleForTesting(5s));

    auto terminated = testing::RunInBackground([&fixture] {
        fixture.session->Terminate();
        return true;
    });
    REQUIRE(terminated.WaitFor(5s));
    CHECK(fixture.session->terminated());
}

TEST_CASE("terminating a session whose subscriber never drains returns instead of deadlocking",
          "[session][invariant]") {
    // THE INVARIANT (S5): Publish is drop-oldest and never blocks, so NO
    // producer -- worker, reader, watcher -- can be parked on a full queue,
    // and a client that stops draining cannot stall the turn. What can still
    // park is a writer in Pop, and Terminate must unwind it: CloseAll closes
    // every subscriber queue drain-first BEFORE the worker join. The old
    // blocking-Push design once joined first and deadlocked; restore that
    // order and this test wedges rather than fails.
    SessionFixture fixture;
    fixture.Script(testing::TextResponse("hi"), {llm::TextDeltaEvent{0, "a chatty response"}});
    fixture.Start();

    // Attach a stream and never drain it -- the missing writer, exactly.
    testing::SessionStream wedged(*fixture.session);
    for (int i = 0; i < 70000; ++i) {
        v1::ServerEvent filler;
        filler.set_session_id("sess-1");
        // Publish always accepts until the broadcaster closes: capacity
        // refusal no longer exists, the queue drops its own oldest instead.
        (void)testing::Transport(*fixture.session).queue().Publish(std::move(filler));
    }
    // The cap is real and reachable, and drop-oldest held it.
    CHECK(wedged.queued() == common::kDefaultEventQueueMaxSize);

    // The turn runs to completion anyway -- the worker is never gated on a
    // client.
    fixture.session->SubmitTurn("hello");
    REQUIRE(fixture.session->WaitForIdleForTesting(5s));

    auto terminated = testing::RunInBackground([&fixture] {
        fixture.session->Terminate();
        return true;
    });
    REQUIRE(terminated.WaitFor(5s));
    CHECK(fixture.session->terminated());
}

// Emits one output chunk -- a publish into whatever queue state the test set
// up -- and then parks on the turn's cancel token until cancelled. The park
// makes the cancel rendezvous deterministic: the loop cannot proceed to its
// next provider call while the tool is parked, so Cancel always lands mid-turn.
class EmittingTool : public tools::Tool {
public:
    EmittingTool(std::string name, std::atomic<bool>& started, std::atomic<bool>& finished)
        : started_(started), finished_(finished) {
        spec_.name = std::move(name);
        spec_.category = tools::ToolCategory::ReadOnly;
        spec_.definition.name = spec_.name;
        spec_.definition.description = "emits one output chunk, then parks until cancelled";
        spec_.definition.input_schema = nlohmann::json::object();
    }

    const tools::ToolSpec& spec() const override { return spec_; }

    tools::ToolResult execute(const nlohmann::json&, tools::ToolContext& ctx) override {
        started_.store(true, std::memory_order_release);
        if (ctx.emit_output) ctx.emit_output("chunk");  // must not park, whatever the queue
        emitted_.store(true, std::memory_order_release);
        if (ctx.cancel_token != nullptr) {
            std::unique_lock<std::mutex> lock(ctx.cancel_token->Mu());
            ctx.cancel_token->Cv().wait(lock, [&] { return ctx.cancel_token->IsCancelled(); });
        }
        finished_.store(true, std::memory_order_release);
        return {false, "done"};
    }

    bool emitted() const { return emitted_.load(std::memory_order_acquire); }

private:
    std::atomic<bool>& started_;
    std::atomic<bool>& finished_;
    std::atomic<bool> emitted_{false};
    tools::ToolSpec spec_;
};

TEST_CASE("a turn runs and cancels cleanly while the subscriber queue is full",
          "[session][invariant]") {
    // THE INVARIANT (S5): nothing a CLIENT does -- including never draining --
    // can park a producer. The tool's emit_output publishes into a full queue,
    // which drops its own oldest and keeps going, so the tool finishes on its
    // own and a cancel reaches the turn through the generation/token path
    // alone. The old version of this test asserted the queue unwedge
    // (AbortWaitingProducers) that made that reachable; drop-oldest publish
    // removed the park and the invariant became structural. What must still
    // hold: the turn ends with its TurnCompleted(Cancelled), and the session
    // lives on.
    SessionFixture fixture;
    std::atomic<bool> started{false};
    std::atomic<bool> finished{false};
    auto tool = std::make_unique<EmittingTool>("spew", started, finished);
    auto* tool_ptr = tool.get();
    fixture.tools_registry.Register(std::move(tool));
    fixture.Script(testing::ToolUseResponse("call-1", "spew"));
    fixture.Start();

    testing::SessionStream wedged(*fixture.session);
    for (int i = 0; i < 70000; ++i) {
        v1::ServerEvent filler;
        filler.set_session_id("sess-1");
        (void)testing::Transport(*fixture.session).queue().Publish(std::move(filler));
    }

    fixture.session->SubmitTurn("go");
    const auto started_deadline = std::chrono::steady_clock::now() + 5s;
    while (!started.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < started_deadline) {
        std::this_thread::sleep_for(1ms);
    }
    REQUIRE(started.load(std::memory_order_acquire));  // the tool ran; its chunk publish dropped
    // Give the chunk publish a moment to have returned: the tool parks only
    // AFTER emit_output comes back, so reaching `emitted` proves the publish
    // into the full queue did not park.
    const auto emitted_deadline = std::chrono::steady_clock::now() + 5s;
    while (!tool_ptr->emitted() && std::chrono::steady_clock::now() < emitted_deadline) {
        std::this_thread::sleep_for(1ms);
    }
    CHECK(tool_ptr->emitted());

    fixture.session->Cancel();

    // The parked tool wakes on the cancel token, the turn unwinds, and the
    // TurnCompleted carrying Cancelled is published into the still-full
    // queue -- drop-oldest makes room, so the terminator always lands.
    const auto finished_deadline = std::chrono::steady_clock::now() + 5s;
    while (!finished.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < finished_deadline) {
        std::this_thread::sleep_for(1ms);
    }
    CHECK(finished.load(std::memory_order_acquire));

    std::optional<v1::StopReason> observed;
    const auto drained_deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < drained_deadline) {
        for (const auto& event : wedged.Drain()) {
            if (event.has_turn_completed()) observed = event.turn_completed().stop_reason();
        }
        if (observed.has_value()) break;
        std::this_thread::sleep_for(1ms);
    }
    REQUIRE(observed.has_value());
    CHECK(*observed == v1::STOP_REASON_CANCELLED);
    CHECK_FALSE(fixture.session->terminated());  // cancel ends the turn, not the session
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
    rpc::FinishSessionWiring(params);

    agent::Session session(std::move(params), fixture.preset, std::move(provider),
                           std::move(store), fixture.tools_registry, fixture.loop_config,
                           fixture.observers, common::DefaultLogger());

    session.SubmitTurn("hello");
    REQUIRE(session.WaitForIdleForTesting(5s));
    CHECK_FALSE(session.terminated());  // one degraded turn, a live session

    bool saw_error = false;
    for (const auto& event : testing::DrainEvents(session)) {
        if (event.has_error()) saw_error = true;
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

TEST_CASE("concurrent Terminate calls do not double-join the worker", "[session][invariant]") {
    // The defect this guards is UB (two threads calling worker_.join() on the
    // same std::thread concurrently), not a wrong return value -- a plain run
    // can pass by luck. This test's job is to give tsan something to look at.
    SessionFixture fixture;
    fixture.Script(testing::TextResponse("hi"));
    fixture.Start();

    constexpr int kCallers = 8;
    std::atomic<int> ready{0};
    std::vector<std::thread> threads;
    threads.reserve(kCallers);
    for (int i = 0; i < kCallers; ++i) {
        threads.emplace_back([&] {
            ready.fetch_add(1);
            while (ready.load() < kCallers) {
            }
            fixture.session->Terminate();
        });
    }
    for (auto& t : threads) t.join();

    CHECK(fixture.session->terminated());
}

TEST_CASE("the queue closes on termination so a writer can finish", "[session]") {
    SessionFixture fixture;
    fixture.Script(testing::TextResponse("hi"));
    fixture.Start();
    fixture.session->SubmitTurn("hello");
    REQUIRE(fixture.session->WaitForIdleForTesting(5s));

    testing::SessionStream stream(*fixture.session);
    fixture.session->Terminate();
    CHECK(testing::Transport(*fixture.session).queue().closed());

    // Drains to exhaustion and then reports closed, rather than blocking.
    while (stream.Pop().has_value()) {
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

TEST_CASE(
    "a turn submitted to a finished subagent is refused with a visible error, not silently "
    "dropped",
    "[session][regression]") {
    // The bug: SubmitTurn's terminated_/subagent_finished_ early returns used
    // to `return false` with no EmitEvent, unlike the backlog-full refusal a
    // few lines below -- breaking session.hpp's documented contract that
    // false always comes with an error on the wire. A subagent's single turn
    // finishing (on_turn_finished fires, subagent_finished_ flips true) is a
    // normal, non-racy way to reach that state without tearing down the
    // session or its transport, so it is directly testable here.
    SessionFixture fixture;
    fixture.on_turn_finished = [](const agent::TurnResult&) {};
    fixture.Script(testing::TextResponse("done"));
    fixture.Start();

    fixture.session->SubmitTurn("the only turn");
    REQUIRE(fixture.session->WaitForIdleForTesting(5s));
    REQUIRE(fixture.session->subagent_finished());

    CHECK_FALSE(fixture.session->SubmitTurn("too late"));

    bool saw_refusal = false;
    for (const auto& event : fixture.DrainQueue()) {
        if (event.has_error() && event.error().message().find("finished") != std::string::npos) {
            saw_refusal = true;
        }
    }
    CHECK(saw_refusal);
}

TEST_CASE(
    "a command submitted to a terminated session gets a CommandResult, not a permanent hang",
    "[session][regression]") {
    // Same bug, the command side: SubmitCommand's terminated_ early return
    // used to skip RejectCommand entirely, unlike its own backlog-full
    // sibling three lines below. A client waiting on this request_id's
    // CommandResult must not hang.
    //
    // A real transport is built (so EmitEvent has somewhere to land, and this
    // test's DrainQueue can observe it) but its Close is overridden to a
    // no-op: this reproduces the window the real bug lives in
    // (SessionRegistry::Terminate erases the registry entry and flips
    // terminated_ before Session::Terminate gets around to closing the
    // transport) deterministically instead of needing the real race.
    SessionFixture fixture;
    class NeverCloses : public rpc::EventTranslator {
    public:
        using EventTranslator::EventTranslator;
        void Close() override {}
    };
    fixture.injected_transport =
        std::make_shared<NeverCloses>("sess-1", rpc::MakeTransportChannels("sess-1"));
    fixture.Script(testing::TextResponse("hi"));
    fixture.Start();
    fixture.session->Terminate();

    agent::QueuedCommand command;
    command.request_id = "req-1";
    command.name = "cost";
    CHECK_FALSE(fixture.session->SubmitCommand(command));

    bool saw_result = false;
    for (const auto& event : fixture.DrainQueue()) {
        if (event.has_command_result() && event.command_result().request_id() == "req-1") {
            saw_result = true;
        }
    }
    CHECK(saw_result);
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
    rpc::FinishSessionWiring(params);

    auto owned_store = std::make_unique<testing::InMemoryHistoryStore>();
    auto* store = owned_store.get();
    agent::Session session(std::move(params), fixture.preset, std::move(provider), std::move(owned_store),
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
    tools_bash::BackgroundTaskRegistry background;

    auto run = [&](const std::string& session_id, const std::filesystem::path& root,
                   const std::string& command) {
        SessionFixture fixture(root);
        fixture.tools_registry.Register(
            tools_bash::MakeBashTool(tools_bash::ParseBashSettings(nlohmann::json::object()), store, background));
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
    rpc::FinishSessionWiring(params);

    auto owned_store = std::make_unique<testing::InMemoryHistoryStore>();
    auto* store = owned_store.get();
    agent::Session session(std::move(params), fixture.preset, std::move(provider), std::move(owned_store),
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

    // Order: the injection is flushed at TURN START, so it precedes the seed
    // both in the transcript and in the request the provider saw. (Before the
    // turn-start flush existed, an injection queued while idle landed after
    // the seed and response -- the model never saw it in the turn that should
    // have carried it.)
    const auto& stored = fixture.store->messages;
    REQUIRE(stored.size() == 3);
    REQUIRE(stored[0].role == llm::Role::User);
    REQUIRE(stored[0].content.size() == 1);
    CHECK(std::get<llm::ToolResultBlock>(stored[0].content.front()).tool_use_id == "spawn-1");
    CHECK(stored[1].role == llm::Role::User);       // "next turn"
    CHECK(stored[2].role == llm::Role::Assistant);  // "next answer"
}

TEST_CASE("session cancellation reaches an advisor through the shared cancel source",
          "[session][advisor]") {
    // The producer composes the advised checker before the Session exists, so
    // only the shared AdvisorCancelSource can carry cancellation to the
    // advisor; the Session installs its predicate in its constructor body. If
    // that wiring breaks, the advisor below never observes the cancel and the
    // turn never completes -- WaitForIdleForTesting fails rather than hanging
    // the suite, and teardown still joins cleanly (terminate also cancels).
    SessionFixture fixture;
    fixture.tools_registry.Register(
        std::make_unique<testing::FakeTool>("bash", tools::ToolCategory::Execute, "ran"));
    fixture.preset.permission_mode.execute = tools::PermissionMode::Ask;

    auto cancel_source = std::make_shared<tools::AdvisorCancelSource>();
    struct Polling : tools::PermissionAdvisor {
        std::optional<tools::PermissionResponse> Advise(const tools::PermissionRequest&,
                                                        const tools::AdvisorContext& ctx) override {
            while (!ctx.is_cancelled()) std::this_thread::sleep_for(1ms);
            return std::nullopt;  // abstain on cancellation, per the contract
        }
    };
    fixture.injected_checker = std::make_unique<tools::AdvisedPermissionChecker>(
        std::make_shared<Polling>(), std::make_unique<testing::AlwaysDenyPermissionChecker>(),
        cancel_source);
    fixture.injected_cancel_source = cancel_source;

    fixture.Script(testing::ToolUseResponse("call-1", "bash", {{"command", "ls"}}));
    fixture.Script(testing::TextResponse("after"));
    fixture.Start();

    fixture.session->SubmitTurn("run it");
    std::this_thread::sleep_for(50ms);  // let the worker reach Advise (or not -- both safe)
    fixture.session->Cancel();

    // The advisor abstained on cancellation; the inner checker denied; the turn
    // ENDED rather than wedging on a blocked advisor.
    REQUIRE(fixture.session->WaitForIdleForTesting(5s));
}

TEST_CASE("an injection on an idle session wakes the worker when the setting is on",
          "[session][background]") {
    // background_wake=true: a background task (or subagent) completing while
    // nobody is waiting must still reach the model -- the worker runs a
    // seedless turn whose user content is the injected messages themselves.
    SessionFixture fixture;
    fixture.wake_on_injection = true;
    fixture.Script(testing::TextResponse("build passed, noted"));
    fixture.Start();

    REQUIRE(fixture.session->QueueInjectedMessage(
        llm::Message{llm::Role::User,
                     {llm::ToolResultBlock{"tu-bg", {llm::TextBlock{"[background task bg-1 "
                                                                "completed] build ok"}},
                                           false}}}));

    // A REAL turn ran: provider called, turn completed, history grew. Wait on
    // the CALL COUNT, not on idle -- an idle session is also the state before
    // the worker has picked the wake turn up, and call_count() is the
    // mutex-guarded accessor: requests() itself may only be read once the
    // worker is provably quiescent (the idle wait below).
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (fixture.provider->call_count() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    REQUIRE(fixture.provider->call_count() >= 1);
    REQUIRE(fixture.session->WaitForIdleForTesting(5s));
    const auto& request = fixture.provider->requests().front();
    REQUIRE(request.messages.size() == 1);  // the injection IS the user content
    CHECK(std::get<llm::ToolResultBlock>(request.messages[0].content.front()).tool_use_id ==
          "tu-bg");

    // The transcript records it as the appended message it is.
    const auto& stored = fixture.store->messages;
    REQUIRE(stored.size() == 2);
    CHECK(stored[0].role == llm::Role::User);
    CHECK(stored[1].role == llm::Role::Assistant);

    // And the client saw a turn_completed for the wake turn -- a headless
    // client that had closed its side would otherwise hang.
    bool saw_completed = false;
    for (const auto& event : fixture.DrainQueue()) {
        if (event.has_turn_completed()) saw_completed = true;
    }
    CHECK(saw_completed);
}

TEST_CASE("an injection on an idle session does NOT wake the worker by default",
          "[session][background]") {
    // Default off: a wake is a provider call nobody asked for. The message
    // stages; the session stays idle; the next user turn flushes it first.
    SessionFixture fixture;
    fixture.Script(testing::TextResponse("later answer"));
    fixture.Start();

    REQUIRE(fixture.session->QueueInjectedMessage(
        llm::Message{llm::Role::User,
                     {llm::ToolResultBlock{"tu-bg", {llm::TextBlock{"[background task bg-1 "
                                                                "completed] build ok"}},
                                           false}}}));

    // No turn ran for it.
    std::this_thread::sleep_for(100ms);
    CHECK(fixture.provider->requests().empty());
    CHECK(fixture.session->WaitForIdleForTesting(100ms));

    // The next turn flushes it BEFORE the seed -- the ordering the loop-level
    // turn-start flush guarantees.
    fixture.Script(testing::TextResponse("with the background result"));
    fixture.session->SubmitTurn("what happened?");
    REQUIRE(fixture.session->WaitForIdleForTesting(5s));

    const auto& request = fixture.provider->requests().front();
    REQUIRE(request.messages.size() == 2);
    CHECK(std::get<llm::ToolResultBlock>(request.messages[0].content.front()).tool_use_id ==
          "tu-bg");
    CHECK(std::get<llm::TextBlock>(request.messages[1].content.front()).text == "what happened?");
}

TEST_CASE("the injection queue is bounded; over the limit is a refusal, not a queue",
          "[session][background][invariant]") {
    // The work queue (32) and the outbound event queue are documented bounded;
    // this third queue silently was not -- producers are non-blocking subagent
    // completions and background task results (up to 1 MB each) landing on an
    // IDLE parent with wake_on_injection off, which flushes only at the next
    // turn. Over the limit is a refusal the caller logs, the same contract
    // Close has.
    agent::InjectionQueue queue;
    for (std::size_t i = 0; i < agent::kMaxPendingInjections; ++i) {
        REQUIRE(queue.Push(llm::Message{llm::Role::User, {llm::TextBlock{"m"}}}));
    }
    CHECK_FALSE(queue.Push(llm::Message{llm::Role::User, {llm::TextBlock{"overflow"}}}));
    // Drain frees slots.
    std::size_t drained = 0;
    queue.DrainAll([&](llm::Message&&) { ++drained; });
    CHECK(drained == agent::kMaxPendingInjections);
    CHECK(queue.Push(llm::Message{llm::Role::User, {llm::TextBlock{"after drain"}}}));
}

TEST_CASE("SetRunMode applies immediately and announces the mode", "[session][run_mode]") {
    // shift+tab's wire path: the flip is reader-side now (Session::SetRunMode,
    // the same contract as SetPaused), so the executor's mode moves the moment
    // the event lands -- no queued work item, no waiting for a turn boundary.
    // The SessionMetadataEvent it publishes is what every attached client's
    // {permission_mode} segment refreshes from -- without it the press LOOKED
    // like a no-op.
    SessionFixture fixture;
    fixture.Script(testing::TextResponse("done"));
    fixture.Start();

    REQUIRE(fixture.session->SetRunMode(tools::PermissionRunMode::Bypass));
    CHECK(fixture.session->run_mode() == tools::PermissionRunMode::Bypass);
    CHECK(fixture.session->desired_run_mode() == tools::PermissionRunMode::Bypass);

    bool announced = false;
    for (const auto& event : fixture.DrainQueue()) {
        if (event.has_session_metadata() &&
            event.session_metadata().permission_mode() == "bypass") {
            announced = true;
        }
    }
    CHECK(announced);
}

TEST_CASE("a mid-turn run-mode switch gates the turn's very next tool call",
          "[session][run_mode][invariant]") {
    // THE BUG THIS FIXES: the switch used to queue a work item, so a shift+tab
    // pressed while the agent sat on a gated tool call applied only at the turn
    // boundary -- the operator's "let it through" did nothing for the turn they
    // pressed it during, and the SAME turn re-prompted for the next call. The
    // flip must be visible to the executor while the worker is parked on the
    // prompt, so the next round's gate sees Bypass and does not ask.
    SessionFixture fixture;
    fixture.preset.permission_mode = testing::AllowAllPolicy();
    fixture.preset.permission_mode.execute = tools::PermissionMode::Ask;
    auto tool = std::make_unique<testing::FakeTool>("bash", tools::ToolCategory::Execute, "ran");
    auto* tool_ptr = tool.get();
    fixture.tools_registry.Register(std::move(tool));

    fixture.Script(testing::ToolUseResponse("call-1", "bash"));  // parks the worker on the Ask
    fixture.Script(testing::ToolUseResponse("call-2", "bash"));  // the SAME turn's next round
    fixture.Script(testing::TextResponse("done"));
    fixture.Start();

    fixture.session->SubmitTurn("go");
    const auto request_id = WaitForPermissionRequest(fixture);
    REQUIRE(request_id.has_value());  // worker parked on the prompt

    // Reader-side flip, exactly what the SetRunModeEvent handler does.
    REQUIRE(fixture.session->SetRunMode(tools::PermissionRunMode::Bypass));
    CHECK(fixture.session->run_mode() == tools::PermissionRunMode::Bypass);

    // An open Ask is NOT retroactively answered by the switch: the operator
    // still answers this one; every later gate sees the new mode.
    REQUIRE(testing::Transport(*fixture.session)
                .pending()
                .Resolve(*request_id, rpc::PermissionAnswer{tools::PermissionDecision::Allow, {}}));
    REQUIRE(fixture.session->WaitForIdleForTesting(5s));

    // The second call of the SAME turn ran unasked: two rounds + the closing
    // text against two tool executions, and no second PermissionRequest on the
    // wire. Under the old queued switch this turn would have prompted again.
    CHECK(fixture.provider->call_count() == 3);
    // Post-idle read: the worker joined the wait predicate, so its writes are
    // visible (the same happens-before edge every other post-turn check uses).
    CHECK(tool_ptr->calls.size() == 2);
    int asks = 0;
    for (const auto& event : fixture.DrainQueue()) {
        if (event.has_permission_request()) ++asks;
    }
    CHECK(asks == 0);
    CHECK(fixture.session->run_mode() == tools::PermissionRunMode::Bypass);
}

TEST_CASE("a borrowed turn does not revert a SetRunMode switch", "[session][run_mode]") {
    // The regression, twice over: PresetRestore first recomputed the executor's
    // run mode from params_/preset_ -- neither of which a shift+tab change
    // writes -- so the next borrowed turn silently reverted the user's switch.
    // It then snapshot-at-pop, which a reader-side flip landing DURING a
    // borrowed turn would race the same way. The restore now reads
    // desired_run_mode_ at restore time: the user's latest word outranks the
    // borrow, whenever it landed.
    testing::TempDir dir;
    std::ofstream(dir.path() / "settings.json")
        << "{\"default_model\": \"main/test-model\"}\n";
    std::ofstream(dir.path() / "permissions.json") << "{}\n";
    // One provider + one model, so the borrow's model resolution succeeds and
    // the turn reaches the scripted provider (an empty providers.json throws
    // at resolve time and the turn fails before the borrow applies).
    std::ofstream(dir.path() / "providers.json")
        << R"({"providers":{"main":{"backend":"openai",)"
           R"("url":"http://localhost/v1","models":{"test-model":{}}}}})"
        << "\n";
    std::filesystem::create_directories(dir.path() / "agents");
    // Same provider/model as the fixture's preset: a PURE borrow, so the
    // provider-rebind path stays out of it and the test isolates the run-mode
    // restore.
    std::ofstream(dir.path() / "agents" / "test.md")
        << "---\nname: test\nmodel: main/test-model\n---\nborrow body\n";
    agent::AgentPresetRegistry registry(
        dir.path(), config::LoadSettingsConfig(dir.path() / "settings.json"));

    SessionFixture fixture(dir.path());
    fixture.preset_registry = &registry;
    // Run the session on the registry's OWN resolved preset: the pop-time
    // transient is registry.Resolve("test") verbatim, so a session preset that
    // matches it keeps the provider-rebind path out of the test and isolates
    // the run-mode restore. The fixture's tool/policy fields are re-applied on
    // top, as the default fixture preset would have carried them.
    fixture.preset = registry.Resolve("test");
    fixture.preset.allowed_tools = {"*"};
    fixture.preset.permission_mode = testing::AllowAllPolicy();
    fixture.preset.run_mode = tools::PermissionRunMode::Normal;
    fixture.Script(testing::TextResponse("borrowed"));
    fixture.Start();

    REQUIRE(fixture.session->SetRunMode(tools::PermissionRunMode::AllowOnly));
    REQUIRE(fixture.session->run_mode() == tools::PermissionRunMode::AllowOnly);

    agent::QueuedTurn borrowed;
    borrowed.text = "run under the override";
    borrowed.agent_preset = "test";  // the same preset: a pure borrow
    REQUIRE(fixture.session->SubmitTurnWithOverrides(borrowed));
    // Mid-borrow: the provider call is in flight, the borrow's transient state
    // is live on the worker. A flip landing NOW is the race the pop-time
    // snapshot lost.
    REQUIRE(WaitForCallCount(*fixture.provider, 1));

    REQUIRE(fixture.session->SetRunMode(tools::PermissionRunMode::Bypass));
    REQUIRE(fixture.session->WaitForIdleForTesting(5s));
    CHECK(fixture.session->run_mode() == tools::PermissionRunMode::Bypass);
    CHECK(fixture.session->desired_run_mode() == tools::PermissionRunMode::Bypass);

    // And a plain turn keeps it too -- the restore must not have leaked the
    // borrow's bookkeeping into the session's sticky state.
    fixture.Script(testing::TextResponse("plain"));
    fixture.session->SubmitTurn("plain");
    REQUIRE(fixture.session->WaitForIdleForTesting(5s));
    CHECK(fixture.session->run_mode() == tools::PermissionRunMode::Bypass);
}

TEST_CASE("a per-turn provider override keeps the borrowed provider alive through the turn",
          "[session][regression]") {
    // loop_->SetProvider stores a REFERENCE, so the borrowed instance must be
    // owned for the whole turn. The assertions cannot observe a use-after-free;
    // under ASan this is the reachable repro of one.
    testing::TempDir dir;
    std::ofstream(dir.path() / "settings.json")
        << "{\"default_model\": \"main/test-model\"}\n";
    std::ofstream(dir.path() / "permissions.json") << "{}\n";
    // Two providers, so the borrow's model resolution picks a DIFFERENT
    // provider entry than the session's own preset -- flipping provider_changed
    // to true and exercising the borrowed-provider construction path.
    std::ofstream(dir.path() / "providers.json")
        << R"({"providers":{)"
           R"("main":{"backend":"fake","url":"http://localhost/v1","models":{"test-model":{}}},)"
           R"("other":{"backend":"fake","url":"http://localhost/v2","models":{"other-model":{}}}}})"
        << "\n";
    std::filesystem::create_directories(dir.path() / "agents");
    std::ofstream(dir.path() / "agents" / "test.md")
        << "---\nname: test\nmodel: main/test-model\n---\nborrow body\n";
    agent::AgentPresetRegistry registry(
        dir.path(), config::LoadSettingsConfig(dir.path() / "settings.json"));

    // Every "fake" provider the factory constructs is scripted to answer once;
    // the fixture's own `provider` (the session's sticky one) is never used
    // once the borrow rebinds.
    llm::ProviderFactoryRegistry factories;
    REQUIRE(factories.Register("fake", [](const llm::ProviderConfig&) {
        auto provider = std::make_unique<testing::FakeLlmProvider>();
        provider->PushResponse(testing::TextResponse("borrowed via factory"));
        return provider;
    }));

    agent::ProviderResolver resolver(registry.providers(), factories, common::DefaultLogger());
    SessionFixture fixture(dir.path());
    fixture.preset_registry = &registry;
    fixture.provider_resolver = &resolver;
    fixture.preset = registry.Resolve("test");
    fixture.preset.allowed_tools = {"*"};
    fixture.preset.permission_mode = testing::AllowAllPolicy();
    fixture.preset.run_mode = tools::PermissionRunMode::Normal;
    fixture.Script(testing::TextResponse("unused"));
    fixture.Start();

    agent::QueuedTurn borrowed;
    borrowed.text = "run on the other provider";
    borrowed.model = "other/other-model";  // a different provider entry -> provider_changed
    REQUIRE(fixture.session->SubmitTurnWithOverrides(borrowed));
    REQUIRE(fixture.session->WaitForIdleForTesting(5s));

    // The turn borrowed a provider through the resolver and ran to completion.
    CHECK(fixture.session->run_mode() == tools::PermissionRunMode::Normal);
}

TEST_CASE("ApplyPreset rebinds when only the provider entry changes",
          "[session][provider][regression]") {
    // Two providers.json entries can share kind, url and key yet own different
    // cached instances (the cache is keyed by entry name, and each instance
    // carries its entry's resolved config). Switching between them must
    // rebind; comparing only the connection fields kept the old instance.
    llm::ProviderFactoryRegistry factories;
    REQUIRE(factories.Register("fake", [](const llm::ProviderConfig&) {
        return std::make_unique<testing::FakeLlmProvider>();
    }));
    config::ProvidersConfig catalog;
    agent::ProviderResolver resolver(catalog, factories, common::DefaultLogger());

    SessionFixture fixture;
    fixture.provider_resolver = &resolver;
    fixture.preset.model_ref = {"main", "m"};
    fixture.preset.provider_kind = "fake";
    fixture.preset.provider.base_url = "http://localhost/v1";
    fixture.Start();
    const auto before = fixture.session->ProviderSnapshot();

    auto switched = fixture.preset;
    switched.model_ref = {"twin", "m"};  // same kind, url and key; different entry
    fixture.session->ApplyPreset(switched);

    const auto after = fixture.session->ProviderSnapshot();
    REQUIRE(after != nullptr);
    CHECK(after != before);
    CHECK(after == resolver.cache().GetOrCreate("twin", "fake", switched.provider));
}

TEST_CASE("SetRunMode is refused on a terminated session and changes nothing",
          "[session][run_mode][invariant]") {
    // The one refusal SetRunMode has (there is no backlog to fill anymore): a
    // terminated session accepts no mode change, and the executor's mode is
    // untouched -- the call cannot half-apply.
    SessionFixture fixture;
    fixture.Start();

    REQUIRE(fixture.session->SetRunMode(tools::PermissionRunMode::AllowOnly));
    CHECK(fixture.session->run_mode() == tools::PermissionRunMode::AllowOnly);

    fixture.session->Terminate();
    CHECK_FALSE(fixture.session->SetRunMode(tools::PermissionRunMode::Bypass));
    CHECK(fixture.session->run_mode() == tools::PermissionRunMode::AllowOnly);
}

TEST_CASE("a preset re-resolution does not reset the runtime run mode",
          "[session][run_mode][regression]") {
    // THE REGRESSION: /model (and /agent) land in Session::ApplyPreset, which
    // recomputed desired_run_mode_ from preset_.run_mode -- the global
    // default stamped on every resolve, since no preset can declare a mode --
    // silently resetting a shift+tab'd bypass back to Normal. A preset swap
    // is a model/policy change, not a posture change; the mode the user set
    // (SetRunMode) and the mode the session was created with
    // (params_.run_mode_override) both survive it.
    SessionFixture fixture;
    fixture.Start();

    REQUIRE(fixture.session->SetRunMode(tools::PermissionRunMode::Bypass));
    CHECK(fixture.session->run_mode() == tools::PermissionRunMode::Bypass);

    auto switched = fixture.preset;
    switched.name = "switched";
    fixture.session->ApplyPreset(switched);

    CHECK(fixture.session->run_mode() == tools::PermissionRunMode::Bypass);
    CHECK(fixture.session->desired_run_mode() == tools::PermissionRunMode::Bypass);
    // The metadata emission still fires (ApplyPreset's contract to every
    // attached client's status bar) -- it must announce the UNCHANGED mode,
    // not a recomputed one.
    bool mode_announced = false;
    for (const auto& event : fixture.DrainQueue()) {
        if (event.has_session_metadata() &&
            event.session_metadata().permission_mode() == "bypass") {
            mode_announced = true;
        }
    }
    CHECK(mode_announced);
}

TEST_CASE("a preset switch installs the new preset's permission policy",
          "[session][permissions][regression]") {
    // THE REGRESSION: the executor was built from the creation-time preset
    // and ApplyPreset never touched it, so `/agent plan` from build kept
    // build's `file_write: ask` -- plan's deny was never enforced.
    SessionFixture fixture;
    fixture.Start();

    auto plan = fixture.preset;
    plan.name = "plan";
    plan.permission_mode.file_write = tools::PermissionMode::AlwaysDeny;
    mcp::McpPermissionPolicy plan_mcp;
    plan_mcp.default_mode = tools::PermissionMode::AlwaysDeny;
    plan.permission_models = {mcp::BindMcpPermissionModel(plan_mcp)};
    fixture.session->ApplyPreset(plan);

    CHECK(fixture.session->policy().file_write == tools::PermissionMode::AlwaysDeny);
    // The plugin models follow the preset too.
    const auto& models = fixture.session->permission_models();
    REQUIRE(models.size() == 1);
    CHECK(static_cast<const mcp::McpPermissionPolicy*>(models.front().layer.get())->default_mode ==
          tools::PermissionMode::AlwaysDeny);
}

TEST_CASE(
    "ApplyPreset rejects the whole switch when the new provider fails to construct",
    "[session][regression]") {
    // The bug: ApplyPreset committed preset_, called loop_->SetPreset, and
    // emitted EmitMetadataChanged() BEFORE attempting the provider rebuild.
    // On rebuild failure, the client had already been told a model switch
    // happened -- and the next turn would send the NEW preset's model name
    // to the OLD provider connection. Fixed: the rebuild now runs first, and
    // a failure leaves preset_/the announced metadata untouched.
    SessionFixture fixture;  // no provider_resolver set: any provider switch fails
    fixture.Start();

    const std::string original_name = fixture.session->info().agent_preset;
    const std::string original_kind = fixture.session->preset_snapshot().provider_kind;

    auto switched = fixture.preset;
    switched.name = "switched-but-should-fail";
    switched.provider_kind = "some-other-backend";  // differs -> provider_changed
    fixture.session->ApplyPreset(switched);

    // Nothing committed: same preset kind, same session-info name.
    CHECK(fixture.session->preset_snapshot().provider_kind == original_kind);
    CHECK(fixture.session->info().agent_preset == original_name);

    // No metadata event announcing the new (never-applied) preset name.
    bool announced_new_name = false;
    for (const auto& event : fixture.DrainQueue()) {
        if (event.has_session_metadata() &&
            event.session_metadata().agent_preset() == "switched-but-should-fail") {
            announced_new_name = true;
        }
    }
    CHECK_FALSE(announced_new_name);
}

TEST_CASE("a session-creation run-mode override survives a preset re-resolution",
          "[session][run_mode]") {
    // The other legitimate writer: params_.run_mode_override (the client's
    // --run-mode / subagent-clamped choice) fixed the mode at construction.
    // ApplyPreset removing its recompute must not free a preset to win over
    // it either -- the mode simply never moves. Built directly (not through
    // the fixture) because the creation override rides SessionParams, the
    // same shape the headless-override test above uses.
    SessionFixture fixture;
    fixture.Script(testing::TextResponse("unused"));

    agent::SessionParams params;
    params.session_id = "sess-creation-override";
    params.agent_preset = "test";
    params.project_root = fixture.project_root;
    params.run_mode_override = tools::PermissionRunMode::Bypass;
    rpc::FinishSessionWiring(params);

    auto owned_store = std::make_unique<testing::InMemoryHistoryStore>();
    agent::Session session(std::move(params), fixture.preset,
                           std::make_unique<testing::FakeLlmProvider>(),
                           std::move(owned_store), fixture.tools_registry, fixture.loop_config,
                           fixture.observers, common::DefaultLogger());

    CHECK(session.run_mode() == tools::PermissionRunMode::Bypass);
    session.ApplyPreset(fixture.preset);
    CHECK(session.run_mode() == tools::PermissionRunMode::Bypass);
    CHECK(session.desired_run_mode() == tools::PermissionRunMode::Bypass);
    session.Terminate();
}

TEST_CASE("ApplyPreset refreshes info_ and announces the new metadata", "[session][metadata]") {
    // /agent and /model land here: without the info_ refresh the tab label and
    // {agent} kept the session's ORIGINAL preset, and without the event every
    // attached client's status bar stayed stale until re-attach.
    SessionFixture fixture;
    fixture.Start();

    auto switched = fixture.preset;
    switched.name = "switched";
    switched.context_window_tokens = 123456;
    fixture.session->ApplyPreset(switched);

    bool announced = false;
    for (const auto& event : fixture.DrainQueue()) {
        if (event.has_session_metadata() &&
            event.session_metadata().agent_preset() == "switched" &&
            event.session_metadata().context_window_tokens() == 123456) {
            announced = true;
        }
    }
    CHECK(announced);
    CHECK(fixture.session->info().agent_preset == "switched");
}

// --- pause (SetPausedEvent) and real-time steering ----------------------------
//
// The fence semantics: pause fences NEW work only. The tool round in flight
// when the pause lands runs to completion and persists its REAL results; the
// turn ends STOP_REASON_PAUSED at the tool-call-return boundary and no further
// provider call happens. Resume (explicit or via a user message) continues
// from the paired transcript.

namespace {

bool WaitForToolStarted(std::atomic<bool>& started, std::chrono::milliseconds timeout = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!started.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    return started.load(std::memory_order_acquire);
}

}  // namespace

TEST_CASE("pause ends the turn at the tool-call return with real results persisted",
          "[session][pause]") {
    SessionFixture fixture;
    std::atomic<bool> started{false};
    auto tool = std::make_unique<testing::GatedTool>("gate", started);
    auto* tool_ptr = tool.get();
    fixture.tools_registry.Register(std::move(tool));
    fixture.Script(testing::ToolUseResponse("call-1", "gate"));
    fixture.Script(testing::TextResponse("must not be consumed"));
    fixture.Start();

    fixture.session->SubmitTurn("go");
    REQUIRE(WaitForToolStarted(started));  // the worker is parked inside the tool
    fixture.session->SetPaused(true);
    CHECK(fixture.session->paused());
    tool_ptr->Release();
    REQUIRE(fixture.session->WaitForIdleForTesting(5s));

    // The in-flight round completed and persisted; the loop never reached the
    // second provider call.
    CHECK(fixture.provider->call_count() == 1);
    const auto& stored = fixture.store->messages;
    REQUIRE(stored.size() == 3);  // user, assistant(tool_use), user(tool_result)
    CHECK(stored.back().role == llm::Role::User);
    REQUIRE(std::holds_alternative<llm::ToolResultBlock>(stored.back().content.front()));
    CHECK_FALSE(fixture.session->terminated());

    // The wire: the turn reports PAUSED and the metadata carries the state.
    // One pass over one drain: the metadata event precedes TurnCompleted on
    // the wire (SetPaused emits it before the turn ends), so a second drain
    // here would only see what the first already consumed.
    std::optional<v1::StopReason> stop;
    bool saw_paused_metadata = false;
    for (const auto& event : fixture.DrainQueue()) {
        if (event.has_turn_completed()) stop = event.turn_completed().stop_reason();
        if (event.has_session_metadata() && event.session_metadata().paused()) {
            saw_paused_metadata = true;
        }
    }
    REQUIRE(stop.has_value());
    CHECK(*stop == v1::STOP_REASON_PAUSED);
    CHECK(saw_paused_metadata);
}

TEST_CASE("an explicit resume runs the seedless continuation from the persisted results",
          "[session][pause]") {
    SessionFixture fixture;
    std::atomic<bool> started{false};
    auto tool = std::make_unique<testing::GatedTool>("gate", started);
    auto* tool_ptr = tool.get();
    fixture.tools_registry.Register(std::move(tool));
    fixture.Script(testing::ToolUseResponse("call-1", "gate"));
    fixture.Script(testing::TextResponse("continued after resume"));
    fixture.Start();

    fixture.session->SubmitTurn("go");
    REQUIRE(WaitForToolStarted(started));
    fixture.session->SetPaused(true);
    tool_ptr->Release();
    REQUIRE(fixture.session->WaitForIdleForTesting(5s));
    REQUIRE(fixture.provider->call_count() == 1);

    // Unpause with nothing queued: the resume kick is the seedless wake turn.
    fixture.session->SetPaused(false);
    CHECK_FALSE(fixture.session->paused());
    // The worker is parked when the notify lands, so WaitForIdle would return
    // before it even wakes -- poll the provider instead.
    REQUIRE(WaitForCallCount(*fixture.provider, 2));

    CHECK(fixture.provider->call_count() == 2);
    // The call count proves the continuation STARTED, not that its appends
    // finished -- the assistant reply lands after the provider returns. Quiesce
    // the worker, then read the store through its mutexed snapshot (the raw
    // `messages` vector is exactly the race the fake's header warns about).
    REQUIRE(fixture.session->WaitForIdleForTesting(5s));
    const auto stored = fixture.store->Snapshot();
    REQUIRE(stored.size() == 4);
    CHECK(stored.back().role == llm::Role::Assistant);
    // The continuation's request carried the persisted tool results -- the
    // model sees what the paused turn did.
    const auto& request = fixture.provider->requests()[1];
    REQUIRE(request.messages.size() == 3);
    CHECK(std::holds_alternative<llm::ToolResultBlock>(request.messages.back().content.front()));
}

TEST_CASE("a user submission auto-resumes a paused session", "[session][pause]") {
    SessionFixture fixture;  // pause_auto_resume_on_submit defaults to on
    fixture.Script(testing::TextResponse("hello there"));
    fixture.Start();

    fixture.session->SetPaused(true);
    CHECK(fixture.session->paused());
    fixture.session->SubmitTurn("anyone there?");
    REQUIRE(fixture.session->WaitForIdleForTesting(5s));

    CHECK_FALSE(fixture.session->paused());
    CHECK(fixture.provider->call_count() == 1);
    const auto& stored = fixture.store->messages;
    REQUIRE(stored.size() == 2);
    CHECK(stored.back().role == llm::Role::Assistant);
}

TEST_CASE("a paused session holds turns and wakes but still runs commands",
          "[session][pause]") {
    // The "pause to reconfigure" affordance: /model, /agent and rewinds are
    // non-turn work the worker keeps running; turns and wake-turns wait.
    SessionFixture fixture;
    fixture.pause_auto_resume_on_submit = false;  // provable hold
    fixture.wake_on_injection = true;             // ...and the wake holds too
    fixture.Script(testing::TextResponse("held until resume"));
    fixture.Start();

    fixture.session->SetPaused(true);
    REQUIRE(fixture.session->SubmitTurn("held"));
    std::this_thread::sleep_for(50ms);
    CHECK(fixture.provider->call_count() == 0);  // held, not run

    // A command still executes while the turn is held (commands_ is null in
    // this fixture, so the answer is the "not available" error -- but that is
    // the command RUNNING, on the worker, beside the held turn).
    agent::QueuedCommand command;
    command.request_id = "cmd-pause";
    command.name = "anything";
    REQUIRE(fixture.session->SubmitCommand(command));
    bool answered = false;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!answered && std::chrono::steady_clock::now() < deadline) {
        for (const auto& event : fixture.DrainQueue()) {
            if (event.has_command_result() &&
                event.command_result().request_id() == "cmd-pause") {
                answered = true;
            }
        }
        std::this_thread::sleep_for(1ms);
    }
    CHECK(answered);
    CHECK(fixture.provider->call_count() == 0);  // the held turn is still held

    // Unpause releases the held turn.
    fixture.session->SetPaused(false);
    REQUIRE(WaitForCallCount(*fixture.provider, 1));
    // The call count proves the turn STARTED, not that its appends finished --
    // quiesce the worker, then read the store through its mutexed snapshot.
    REQUIRE(fixture.session->WaitForIdleForTesting(5s));
    CHECK(fixture.store->Snapshot().size() == 2);
}

TEST_CASE("cancel beats pause", "[session][pause]") {
    SessionFixture fixture;
    std::atomic<bool> started{false};
    auto tool = std::make_unique<testing::GatedTool>("gate", started);
    fixture.tools_registry.Register(std::move(tool));
    fixture.Script(testing::ToolUseResponse("call-1", "gate"));
    fixture.Script(testing::TextResponse("must not be consumed"));
    fixture.Start();

    fixture.session->SubmitTurn("go");
    REQUIRE(WaitForToolStarted(started));
    fixture.session->SetPaused(true);
    // The cancel fires the tool's token, which wakes the gated tool; the turn
    // unwinds as CANCELLED and the pause state is gone with it.
    fixture.session->Cancel();
    REQUIRE(fixture.session->WaitForIdleForTesting(5s));

    CHECK_FALSE(fixture.session->paused());
    CHECK(fixture.provider->call_count() == 1);
    // One drain, both assertions: the metadata (paused=false, from Cancel)
    // precedes TurnCompleted on the wire.
    std::optional<v1::StopReason> stop;
    for (const auto& event : fixture.DrainQueue()) {
        if (event.has_turn_completed()) stop = event.turn_completed().stop_reason();
    }
    REQUIRE(stop.has_value());
    CHECK(*stop == v1::STOP_REASON_CANCELLED);
}

TEST_CASE("a paused session holds the injection wake until resume", "[session][pause]") {
    SessionFixture fixture;
    fixture.wake_on_injection = true;
    fixture.Script(testing::TextResponse("wake answer"));
    fixture.Start();

    fixture.session->SetPaused(true);
    REQUIRE(fixture.session->QueueInjectedMessage(
        llm::Message{llm::Role::User,
                     {llm::ToolResultBlock{"bg-1", {llm::TextBlock{"task finished"}}, false}}}));
    std::this_thread::sleep_for(50ms);
    CHECK(fixture.provider->call_count() == 0);  // no provider call nobody asked for

    fixture.session->SetPaused(false);
    REQUIRE(WaitForCallCount(*fixture.provider, 1));  // the seedless wake ran on resume
}

TEST_CASE("a mid-turn submission steers into the running turn's next request",
          "[session][steering]") {
    SessionFixture fixture;
    std::atomic<bool> started{false};
    auto tool = std::make_unique<testing::GatedTool>("gate", started);
    auto* tool_ptr = tool.get();
    fixture.tools_registry.Register(std::move(tool));
    fixture.Script(testing::ToolUseResponse("call-1", "gate"));
    fixture.Script(testing::TextResponse("after tools"));
    fixture.Start();

    fixture.session->SubmitTurn("go");
    REQUIRE(WaitForToolStarted(started));  // mid-turn, provably
    REQUIRE(fixture.session->SubmitTurn("steer me"));
    // The steering answer: accepted with steered=true, not a queue row.
    bool saw_steered = false;
    const auto queued_deadline = std::chrono::steady_clock::now() + 5s;
    while (!saw_steered && std::chrono::steady_clock::now() < queued_deadline) {
        for (const auto& event : fixture.DrainQueue()) {
            if (event.has_turn_queued() && event.turn_queued().steered()) saw_steered = true;
        }
        std::this_thread::sleep_for(1ms);
    }
    CHECK(saw_steered);

    tool_ptr->Release();
    REQUIRE(fixture.session->WaitForIdleForTesting(5s));

    // The steering text reached the model IN the round after the tool
    // results: [..., assistant(tool_use), user(tool_results), user(steering)]
    // in one request.
    CHECK(fixture.provider->call_count() == 2);
    const auto& request = fixture.provider->requests()[1];
    REQUIRE(request.messages.size() == 4);
    CHECK(request.messages.back().role == llm::Role::User);
    const auto* text = std::get_if<llm::TextBlock>(&request.messages.back().content.front());
    REQUIRE(text != nullptr);
    CHECK(text->text == "steer me");
    // ...and it persisted at the boundary, keeping the transcript paired.
    const auto& stored = fixture.store->messages;
    REQUIRE(stored.size() == 5);
    const auto* stored_text =
        std::get_if<llm::TextBlock>(&stored[3].content.front());
    REQUIRE(stored_text != nullptr);
    CHECK(stored_text->text == "steer me");
}

TEST_CASE("a steering message that missed the flush is answered by a seedless continuation",
          "[session][steering]") {
    // The model had already returned EndTurn by the time the user typed; the
    // final flush appends the message but nothing answers it. The worker's
    // wait predicate treats a pending user-text injection as explicit intent
    // and runs a seedless continuation -- regardless of wake_on_injection.
    SessionFixture fixture;
    std::atomic<bool> started{false};
    auto tool = std::make_unique<testing::GatedTool>("gate", started);
    auto* tool_ptr = tool.get();
    fixture.tools_registry.Register(std::move(tool));
    fixture.Script(testing::ToolUseResponse("call-1", "gate"));
    fixture.Script(testing::TextResponse("done before you typed"));
    fixture.Script(testing::TextResponse("answering the steering"));
    fixture.Start();

    fixture.session->SubmitTurn("go");
    REQUIRE(WaitForToolStarted(started));
    tool_ptr->Release();
    REQUIRE(fixture.session->WaitForIdleForTesting(5s));
    REQUIRE(fixture.provider->call_count() == 2);

    // Arrives after the turn ended: the flush already ran, so this rides the
    // sweep. (Session::QueueInjectedMessage is exactly what the routing in
    // SubmitTurnWithOverrides would have produced.)
    REQUIRE(fixture.session->QueueInjectedMessage(testing::UserText("late steering")));
    REQUIRE(WaitForCallCount(*fixture.provider, 3));

    CHECK(fixture.provider->call_count() == 3);
    // The call count proves the continuation STARTED, not that its appends
    // finished -- quiesce the worker, then read the store through its mutexed
    // snapshot (the raw `messages` vector is the race the fake's header warns
    // about).
    REQUIRE(fixture.session->WaitForIdleForTesting(5s));
    const auto& request = fixture.provider->requests()[2];
    REQUIRE_FALSE(request.messages.empty());
    const auto* text = std::get_if<llm::TextBlock>(&request.messages.back().content.front());
    REQUIRE(text != nullptr);
    CHECK(text->text == "late steering");
    const auto stored = fixture.store->Snapshot();
    REQUIRE(stored.size() == 6);
    CHECK(stored.back().role == llm::Role::Assistant);
}
