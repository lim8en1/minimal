#include <sys/stat.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "deadline.hpp"
#include "fakes.hpp"
#include "session_events.hpp"
#include "minimal/agent/session.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/hooks/hook_dispatcher.hpp"
#include "minimal/rpc/session_wiring.hpp"

using namespace minimal;
using namespace std::chrono_literals;

namespace {

// The same scaffolding session_test.cpp uses, trimmed to what the
// user_prompt_submit seam needs: a session on a fake store with a hook
// dispatcher wired through LoopConfig::hooks -- the field the registry
// funnel would set in production.
struct HookSessionFixture {
    explicit HookSessionFixture(const std::filesystem::path& dir) : observers(common::DefaultLogger()) {
        preset.name = "test";
        preset.allowed_tools = {"*"};
        preset.permission_mode = testing::AllowAllPolicy();
        preset.run_mode = tools::PermissionRunMode::Normal;
        block_script = dir / "block_prompt.sh";
        std::ofstream out(block_script);
        out << "echo 'prompt not allowed'\nexit 2\n";
        ::chmod(block_script.string().c_str(), 0755);
    }

    void ConfigureBlockingHook() {
        minimal::config::HookActionConfig action;
        action.on = minimal::hooks::kUserPromptSubmit;
        action.command = block_script.string();
        minimal::config::HooksConfig config;
        config.hooks.push_back(std::move(action));
        dispatcher = std::make_unique<minimal::hooks::HookDispatcher>(common::DefaultLogger());
        (void)dispatcher->Configure(config);
    }

    // Scripted BEFORE Start(): the fake provider consumes one step per
    // provider call, in order.
    void Script(llm::ChatResponse response, std::vector<llm::StreamEvent> events = {}) {
        scripted.emplace_back(std::move(response), std::move(events));
    }

    void Start(std::string parent_session_id = "") {
        auto owned_provider = std::make_unique<testing::FakeLlmProvider>();
        provider = owned_provider.get();
        for (auto& step : scripted) {
            owned_provider->PushResponse(step.first, step.second);
        }
        auto owned_store = std::make_unique<testing::InMemoryHistoryStore>();
        store = owned_store.get();

        agent::SessionParams params;
        params.session_id = "sess-h";
        params.parent_session_id = std::move(parent_session_id);
        params.name = "hook session";
        params.agent_preset = "test";
        params.project_root = std::filesystem::temp_directory_path();
        loop_config.hooks = dispatcher.get();
        rpc::FinishSessionWiring(params);

        session = std::make_unique<agent::Session>(
            std::move(params), preset, std::move(owned_provider), std::move(owned_store),
            tools_registry, loop_config, observers, common::DefaultLogger());
        stream = std::make_unique<testing::SessionStream>(*session);
    }

    std::vector<v1::ServerEvent> DrainQueue() { return stream->Drain(); }

    ~HookSessionFixture() {
        stream.reset();
        if (session) session->Terminate();
    }

    std::unique_ptr<testing::SessionStream> stream;

    std::filesystem::path block_script;
    std::unique_ptr<minimal::hooks::HookDispatcher> dispatcher;
    agent::AgentPreset preset;
    agent::LoopConfig loop_config;
    tools::ToolRegistry tools_registry;
    agent::SessionObserverRegistry observers;
    std::vector<std::pair<llm::ChatResponse, std::vector<llm::StreamEvent>>> scripted;
    testing::FakeLlmProvider* provider = nullptr;
    testing::InMemoryHistoryStore* store = nullptr;
    std::unique_ptr<agent::Session> session;
};

bool HasErrorEvent(const std::vector<v1::ServerEvent>& events, const std::string& needle) {
    for (const auto& event : events) {
        if (event.has_error() &&
            event.error().message().find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool HasTurnCompleted(const std::vector<v1::ServerEvent>& events) {
    for (const auto& event : events) {
        if (event.has_turn_completed()) return true;
    }
    return false;
}

}  // namespace

TEST_CASE("a user_prompt_submit hook refusal refuses the turn, answers the client, and persists "
          "nothing",
          "[hooks][session]") {
    const auto dir = std::filesystem::temp_directory_path() / "minimal_hooks_session_test";
    std::filesystem::create_directories(dir);
    HookSessionFixture f{dir};
    f.ConfigureBlockingHook();
    // For the SECOND turn (after the refusal): the fake provider is fed in
    // Start(), so both scripts are queued before the session exists. The
    // blocked first prompt consumes none of them.
    f.Script(testing::TextResponse("here we go again"));
    // The veto path must reach the SAME bottom block every other exit runs:
    // an early return once skipped the observer notification there, leaking
    // per-turn observer state on every vetoed prompt.
    struct VetoRecorder : agent::SessionObserver {
        std::atomic<int> completed{0};
        void OnTurnCompleted(const agent::SessionInfo&, const agent::TurnSummary&) override {
            completed.fetch_add(1);
        }
    };
    auto recorder = std::make_shared<VetoRecorder>();
    f.observers.Register(recorder);
    f.Start();

    CHECK(f.session->SubmitTurn("do the thing"));
    CHECK(f.session->WaitForIdleForTesting(5s));
    f.observers.WaitForIdleForTesting();

    const auto events = f.DrainQueue();
    CHECK(HasErrorEvent(events, "prompt rejected by hook: prompt not allowed"));
    // The end-of-turn signal anyway: a headless client reads until
    // TurnCompletedEvent, and a refusal is an exit path.
    CHECK(HasTurnCompleted(events));
    // The observer was told the turn ended, veto or not.
    CHECK(recorder->completed.load() == 1);
    // Nothing was appended to the transcript.
    CHECK(f.store->messages.empty());
    // The provider was never called.
    CHECK(f.provider->call_count() == 0);
    // The session is alive and keeps working: the next prompt is refused the
    // same way (the hook has no filter), answered on the wire, and nothing is
    // persisted or sent anywhere.
    CHECK_FALSE(f.session->terminated());
    CHECK(f.session->SubmitTurn("still here"));
    CHECK(f.session->WaitForIdleForTesting(5s));
    f.observers.WaitForIdleForTesting();
    CHECK(f.provider->call_count() == 0);
    // ...and the second veto was reported to the observer too.
    CHECK(recorder->completed.load() == 2);

    std::filesystem::remove_all(dir);
}

TEST_CASE("a passing user_prompt_submit hook leaves the turn untouched",
          "[hooks][session]") {
    const auto dir = std::filesystem::temp_directory_path() / "minimal_hooks_session_test2";
    std::filesystem::create_directories(dir);
    HookSessionFixture f{dir};
    // No hook configured at all: the seam is a no-op null dispatcher.
    f.Start();
    f.Script(testing::TextResponse("ok"));

    REQUIRE(f.session->SubmitTurn("go"));
    CHECK(f.session->WaitForIdleForTesting(5s));

    const auto events = f.DrainQueue();
    CHECK_FALSE(HasErrorEvent(events, "hook"));
    CHECK(HasTurnCompleted(events));
    CHECK(!f.store->messages.empty());

    std::filesystem::remove_all(dir);
}

TEST_CASE("a subagent's seeded turn does not answer user_prompt_submit",
          "[hooks][session]") {
    const auto dir = std::filesystem::temp_directory_path() / "minimal_hooks_session_test3";
    std::filesystem::create_directories(dir);
    HookSessionFixture f{dir};
    f.ConfigureBlockingHook();
    // parent_session_id set: the seeded task is the parent model's own
    // formulation, not a user prompt.
    f.Start("parent-1");
    f.Script(testing::TextResponse("done"));

    REQUIRE(f.session->SubmitTurn("summarize the repo"));
    CHECK(f.session->WaitForIdleForTesting(5s));

    const auto events = f.DrainQueue();
    CHECK_FALSE(HasErrorEvent(events, "prompt rejected by hook"));
    CHECK(!f.store->messages.empty());  // the turn ran

    std::filesystem::remove_all(dir);
}

TEST_CASE("a steered message crosses the user_prompt_submit veto too", "[hooks][session][steering]") {
    // Steering is the one user-prompt path that does not pop a turn -- it is
    // injected at a tool-result boundary. Without the flush-time gate it would
    // silently bypass a guard hook every turn-pop passes through. The veto
    // drops the message BEFORE it is persisted, answers the wire, and the turn
    // continues.
    const auto dir = std::filesystem::temp_directory_path() / "minimal_hooks_session_test4";
    std::filesystem::remove_all(dir);  // stale counter from a previous run would veto the seed
    std::filesystem::create_directories(dir);
    HookSessionFixture f{dir};
    // A STATEFUL guard: pass the first prompt (the seeded turn pop), block the
    // second (the steered message's flush). A plain block script would veto
    // the seeded turn before the tool ever ran -- which is the turn-pop seam
    // doing its job, not the one under test here.
    {
        std::ofstream out(f.block_script);
        out << "c=$(cat \"$0.count\" 2>/dev/null || echo 0)\n"
               "c=$((c+1))\n"
               "echo $c > \"$0.count\"\n"
               "if [ $c -ge 2 ]; then echo 'steering not allowed'; exit 2; fi\n"
               "exit 0\n";
        out.close();
        ::chmod(f.block_script.string().c_str(), 0755);
    }
    f.ConfigureBlockingHook();
    std::atomic<bool> started{false};
    auto tool = std::make_unique<testing::GatedTool>("gate", started);
    auto* tool_ptr = tool.get();
    f.tools_registry.Register(std::move(tool));
    f.Script(testing::ToolUseResponse("call-1", "gate"));
    f.Script(testing::TextResponse("after tools"));
    f.Start();

    REQUIRE(f.session->SubmitTurn("go"));
    const auto started_deadline = std::chrono::steady_clock::now() + 5s;
    while (!started.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < started_deadline) {
        std::this_thread::sleep_for(1ms);
    }
    REQUIRE(started.load(std::memory_order_acquire));
    REQUIRE(f.session->SubmitTurn("steered text"));  // mid-turn: rides the flush
    tool_ptr->Release();
    CHECK(f.session->WaitForIdleForTesting(5s));

    // Vetoed: the error names the steering path, and the message never
    // reached the provider request or the store.
    const auto events = f.DrainQueue();
    CHECK(HasErrorEvent(events, "steered message rejected by hook"));
    CHECK(f.provider->call_count() == 2);
    const auto& request = f.provider->requests()[1];
    for (const auto& message : request.messages) {
        for (const auto& block : message.content) {
            if (const auto* text = std::get_if<llm::TextBlock>(&block)) {
                CHECK(text->text != "steered text");
            }
        }
    }
    CHECK(f.store->messages.size() == 4);  // user, assistant(tool_use), user(tool_result), assistant
    CHECK(f.store->messages.back().role == llm::Role::Assistant);  // the turn continued

    std::filesystem::remove_all(dir);
}
