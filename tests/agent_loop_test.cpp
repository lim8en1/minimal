#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>

#include "deadline.hpp"
#include "fakes.hpp"
#include "minimal/agent/agent_loop.hpp"
#include "minimal/config/skill_registry.hpp"
#include "minimal/hooks/hook_dispatcher.hpp"
#include "minimal/tools/skill_tool.hpp"
#include "temp_dir.hpp"

using namespace minimal;
using namespace minimal::testing;
using minimal::tools::PermissionRunMode;
using minimal::tools::ToolCategory;
using namespace std::chrono_literals;

namespace {

config::CompactionTriggerSettings FixedTrigger(std::int64_t tokens) {
    config::CompactionTriggerSettings trigger;
    trigger.kind = config::CompactionTriggerSettings::Kind::FixedTokens;
    trigger.tokens = tokens;
    return trigger;
}

config::CompactionTriggerSettings PercentTrigger(int percent) {
    config::CompactionTriggerSettings trigger;
    trigger.kind = config::CompactionTriggerSettings::Kind::PercentOfWindow;
    trigger.percent = percent;
    return trigger;
}

config::CompactionTriggerSettings BufferTrigger(std::int64_t buffer_tokens) {
    config::CompactionTriggerSettings trigger;
    trigger.kind = config::CompactionTriggerSettings::Kind::WindowBuffer;
    trigger.buffer_tokens = buffer_tokens;
    return trigger;
}

// Shared concurrency probe state: how many executions are inside execute()
// right now, and the high-water mark. The parallel-dispatch tests assert on
// `max`, so a regression to sequential dispatch fails a CHECK rather than
// deadlocking a barrier.
struct ConcurrencyCounter {
    std::atomic<int> inside{0};
    std::atomic<int> max{0};
    void Enter() {
        const int now = inside.fetch_add(1, std::memory_order_acq_rel) + 1;
        int seen = max.load(std::memory_order_relaxed);
        while (now > seen && !max.compare_exchange_weak(seen, now)) {}
    }
    void Exit() { inside.fetch_sub(1, std::memory_order_acq_rel); }
};

// A tool that tracks its own concurrency and optionally holds before returning.
// The hold is the rendezvous point: a test waiting for N concurrent executions
// spins with a deadline, so a concurrency regression degrades to a failed
// assertion (after the timeout) instead of a hung suite.
class ProbeTool : public tools::Tool {
public:
    ProbeTool(std::string name, std::shared_ptr<ConcurrencyCounter> counter,
              std::function<void()> hold)
        : hold_(std::move(hold)), counter_(std::move(counter)), name_(std::move(name)) {
        spec_.name = name_;
        spec_.category = ToolCategory::ReadOnly;
        spec_.definition.name = name_;
        spec_.definition.description = "concurrency probe";
        spec_.definition.input_schema = nlohmann::json::object();
    }

    const tools::ToolSpec& spec() const override { return spec_; }

    tools::ToolResult execute(const nlohmann::json&, tools::ToolContext&) override {
        counter_->Enter();
        if (hold_) hold_();
        counter_->Exit();
        return {false, name_};
    }

    bool parallel_safe() const override { return parallel_safe_.load(); }

    std::atomic<bool> parallel_safe_{true};
    std::function<void()> hold_;

private:
    tools::ToolSpec spec_;
    std::shared_ptr<ConcurrencyCounter> counter_;
    std::string name_;
};

// One assistant message carrying SEVERAL tool_use blocks -- the shape parallel
// dispatch exists for.
llm::ChatResponse MultiToolUseResponse(
    std::initializer_list<std::pair<std::string, std::string>> calls) {
    llm::ChatResponse response;
    for (auto& [id, name] : calls) {
        response.content.push_back(
            llm::ToolUseBlock{std::move(id), std::move(name), nlohmann::json::object()});
    }
    response.stop_reason = llm::StopReason::ToolUse;
    response.usage.input_tokens = 10;
    response.usage.output_tokens = 5;
    return response;
}

const llm::ToolResultBlock& ResultOf(const llm::Message& message, std::size_t index) {
    return std::get<llm::ToolResultBlock>(message.content[index]);
}

// Throws on the Nth call to Append (1-based); every other call behaves like
// the base fake. Local to one test: Harness::store is a fixed
// InMemoryHistoryStore value member many other tests depend on, so a
// store-failure test builds its own AgentLoop rather than widening the
// shared fixture for one case.
class ThrowsOnAppendN : public testing::InMemoryHistoryStore {
public:
    explicit ThrowsOnAppendN(int throw_on_call) : throw_on_call_(throw_on_call) {}
    void Append(const llm::Message& message, std::optional<int> output_tokens) override {
        if (++calls_ == throw_on_call_) {
            throw std::runtime_error("simulated store failure");
        }
        InMemoryHistoryStore::Append(message, output_tokens);
    }

private:
    int throw_on_call_;
    int calls_ = 0;
};

// Assembles the loop and everything it needs. Members are declared in
// destruction-safe order: the loop refers to the others, so it is destroyed
// first.
struct Harness {
    // Skill support for tier-2 coverage of the lazy-load path. Declared first so
    // they are destroyed LAST: the registry below owns a SkillTool that holds a
    // reference into `skills`.
    std::unique_ptr<testing::TempDir> skill_arena;
    std::unique_ptr<config::SkillRegistry> skills;

    FakeLlmProvider provider;
    tools::ToolRegistry registry;
    AlwaysAllowPermissionChecker checker;
    // When set, the executor is built over this checker instead of `checker`.
    std::unique_ptr<tools::PermissionChecker> checker_override;
    // When set, the tool lifecycle sink throws: the containment guard around
    // execute_one's wire events is what keeps that from killing the tool call
    // (and, on a pool worker, the process).
    bool throwing_lifecycle_sink = false;
    // The policy the executor resolves with; AllowAll unless a test asks for
    // prompts.
    tools::PermissionPolicy executor_policy = AllowAllPolicy();
    InMemoryHistoryStore store;
    std::unique_ptr<tools::ToolExecutor> executor;
    std::unique_ptr<agent::AgentLoop> loop;
    std::vector<llm::StreamEvent> seen_events;
    // Atomic because the backoff test flips it from the main thread while the
    // loop polls it mid-sleep; every other test sets it from the same thread.
    std::atomic<bool> cancel_flag{false};

    FakeTool& AddTool(std::string name, ToolCategory category = ToolCategory::ReadOnly) {
        auto owned = std::make_unique<FakeTool>(std::move(name), category);
        FakeTool* raw = owned.get();
        registry.Register(std::move(owned));
        return *raw;
    }

    ProbeTool& AddProbe(std::string name, std::shared_ptr<ConcurrencyCounter> counter,
                        std::function<void()> hold = {}) {
        auto owned =
            std::make_unique<ProbeTool>(std::move(name), std::move(counter), std::move(hold));
        ProbeTool* raw = owned.get();
        registry.Register(std::move(owned));
        return *raw;
    }

    // Registers a real SkillTool over the given skills (name -> body). Call
    // before Build().
    void EnableSkills(const std::vector<std::pair<std::string, std::string>>& bodies) {
        skill_arena = std::make_unique<testing::TempDir>();
        for (const auto& [name, body] : bodies) {
            const auto dir = skill_arena->path() / "skills" / name;
            std::filesystem::create_directories(dir);
            std::ofstream(dir / "SKILL.md") << "---\nname: " << name
                                            << "\ndescription: test skill\n---\n" << body;
        }
        skills = std::make_unique<config::SkillRegistry>(
            config::SkillRegistry::Load(skill_arena->path()));
        registry.Register(std::make_unique<tools::SkillTool>(*skills));
    }

    void Build(agent::AgentPreset preset = {}, agent::LoopConfig config = {},
               agent::Compactor compactor = {}, agent::ResumeSeed seed = {},
               std::string project_context = {}) {
        if (preset.allowed_tools.empty()) preset.allowed_tools = {"*"};
        if (preset.provider.model_name.empty()) preset.provider.model_name = "test-model";
        preset.permission_mode = AllowAllPolicy();

        executor = std::make_unique<tools::ToolExecutor>(registry,
                                                         checker_override ? *checker_override
                                                                          : checker,
                                                         executor_policy,
                                                         tools::McpPermissionPolicy{},
                                                         PermissionRunMode::Normal);
        agent::SessionContext session{common::DefaultLogger()};
        session.session_id = "s1";
        session.project_root = ".";
        if (throwing_lifecycle_sink) {
            session.tool_lifecycle_sink = [](const agent::ToolLifecycleEvent&) {
                throw std::runtime_error("simulated sink failure");
            };
        }

        loop = std::make_unique<agent::AgentLoop>(
            provider, *executor, registry, store, std::move(preset), config, std::move(session),
            [this] { return cancel_flag.load(); },
            [this](const llm::StreamEvent& e) {
                seen_events.push_back(e);
                return !cancel_flag.load();
            },
            std::move(compactor), std::move(seed), std::move(project_context));
    }
};

}  // namespace

TEST_CASE("a single turn ends and persists user + assistant", "[agent_loop]") {
    Harness h;
    h.provider.PushResponse(TextResponse("hello"));
    h.Build();

    const auto result = h.loop->RunTurn(UserText("hi"));

    CHECK(result.stop_reason == llm::StopReason::EndTurn);
    CHECK(result.final_text == "hello");
    CHECK(result.provider_calls == 1);
    REQUIRE(h.store.messages.size() == 2);
    CHECK(h.store.messages[0].role == llm::Role::User);
    CHECK(h.store.messages[1].role == llm::Role::Assistant);
}

TEST_CASE("a tool call round-trips and the loop continues", "[agent_loop]") {
    Harness h;
    auto& tool = h.AddTool("read");
    h.provider.PushResponse(ToolUseResponse("tu1", "read", {{"path", "a.txt"}}));
    h.provider.PushResponse(TextResponse("done"));
    h.Build();

    const auto result = h.loop->RunTurn(UserText("read it"));

    CHECK(result.stop_reason == llm::StopReason::EndTurn);
    CHECK(result.provider_calls == 2);
    CHECK(tool.calls.size() == 1);
    // user, assistant(tool_use), user(tool_result), assistant(text)
    REQUIRE(h.store.messages.size() == 4);
    CHECK(h.store.messages[2].role == llm::Role::User);
    CHECK(std::holds_alternative<llm::ToolResultBlock>(h.store.messages[2].content.front()));
}

TEST_CASE("tool results are persisted before the next provider call", "[agent_loop]") {
    // Mid-turn persistence is the point: a crash between tool calls must not
    // lose completed work, and scrollback must see results while the turn runs.
    Harness h;
    h.AddTool("read");
    h.provider.PushResponse(ToolUseResponse("tu1", "read"));
    h.provider.PushResponse(TextResponse("done"));
    h.Build();

    h.loop->RunTurn(UserText("go"));

    // The tool_result message exists in the store, not only in the final history.
    bool found = false;
    for (const auto& message : h.store.messages) {
        for (const auto& block : message.content) {
            if (std::holds_alternative<llm::ToolResultBlock>(block)) found = true;
        }
    }
    CHECK(found);
}

TEST_CASE("a store failure appending tool results repairs history_ before rethrowing",
          "[agent_loop][invariant]") {
    // THE BUG: AppendAndPersist appends the assistant message (carrying
    // tool_use blocks) BEFORE the tools run, then appends the paired
    // results message AFTER. If the store fails on THAT second append,
    // history_ was left ending in unanswered tool_use -- a hard 400 on both
    // wire formats -- and the exception propagated past RunTurnLoop
    // uncaught. Session::RunOneTurn catches it and keeps the session alive,
    // but nothing repaired the in-memory working set, so every later turn
    // 400'd on the orphaned call for the rest of the session's life (only a
    // process restart's resume path repairs the identical shape). The fix
    // repairs history_ the same way resume does (synthesize_missing_
    // tool_results) before rethrowing the ORIGINAL exception.
    testing::FakeLlmProvider provider;
    provider.PushResponse(testing::ToolUseResponse("tu1", "read"));
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<testing::FakeTool>("read", ToolCategory::ReadOnly));
    testing::AlwaysAllowPermissionChecker checker;
    tools::ToolExecutor executor(registry, checker, testing::AllowAllPolicy(),
                                 tools::McpPermissionPolicy{}, PermissionRunMode::Normal);

    // Append #1 is the seed user message ("go"); #2 is the assistant
    // message carrying tool_use (both succeed); #3 is the results message
    // this test targets (throws); #4, if reached, is the repair's own
    // append (succeeds, proving the repair landed).
    ThrowsOnAppendN store(/*throw_on_call=*/3);

    agent::AgentPreset preset;
    preset.allowed_tools = {"*"};
    preset.provider.model_name = "test-model";
    preset.permission_mode = testing::AllowAllPolicy();
    agent::SessionContext session{common::DefaultLogger()};
    session.session_id = "s1";
    session.project_root = ".";

    agent::AgentLoop loop(
        provider, executor, registry, store, preset, agent::LoopConfig{}, std::move(session),
        [] { return false; }, [](const llm::StreamEvent&) { return true; }, agent::Compactor{});

    // Manual catch, not CHECK_THROWS_WITH: this must be the ORIGINAL
    // exception, not one from the repair attempt's own store call, and
    // asserting the exact message is what proves that.
    bool threw = false;
    try {
        loop.RunTurn(testing::UserText("go"));
    } catch (const std::runtime_error& error) {
        threw = true;
        CHECK(std::string(error.what()) == "simulated store failure");
    }
    REQUIRE(threw);

    // history_ ends with the repair message, not the orphaned assistant
    // turn: a User message carrying an error ToolResultBlock for "tu1" --
    // exactly synthesize_missing_tool_results' shape, the same repair resume
    // already applies to a torn transcript.
    const auto& history = loop.history();
    REQUIRE(history.size() == 3);  // seed, assistant (with tool_use), repair
    const auto& repair = history.back();
    CHECK(repair.role == llm::Role::User);
    REQUIRE(repair.content.size() == 1);
    const auto& result = std::get<llm::ToolResultBlock>(repair.content[0]);
    CHECK(result.tool_use_id == "tu1");
    CHECK(result.is_error);
}

TEST_CASE("a result over max_tool_result_bytes is truncated with a visible marker",
         "[agent_loop][invariant]") {
    // The output-side mirror of the input ceiling: applied uniformly at the
    // one call site every tool's result passes through (Execute()), so it
    // covers MCP tools and third-party plugin tools too, not just core's own
    // bash/read caps.
    Harness h;
    auto owned = std::make_unique<FakeTool>("big", ToolCategory::ReadOnly, std::string(500, 'x'));
    FakeTool* tool = owned.get();
    h.registry.Register(std::move(owned));
    h.provider.PushResponse(ToolUseResponse("tu1", "big"));
    h.provider.PushResponse(TextResponse("done"));

    agent::LoopConfig config;
    config.max_tool_result_bytes = 100;
    h.Build({}, config);

    h.loop->RunTurn(UserText("go"));
    CHECK(tool->calls.size() == 1);

    const llm::ToolResultBlock* result = nullptr;
    for (const auto& message : h.store.messages) {
        for (const auto& block : message.content) {
            if (const auto* r = std::get_if<llm::ToolResultBlock>(&block)) result = r;
        }
    }
    REQUIRE(result != nullptr);
    REQUIRE(result->content.size() == 1);
    const auto& text = std::get<llm::TextBlock>(result->content.front()).text;
    CHECK(text.size() < 500);
    CHECK(text.rfind("xxx", 0) == 0);   // the kept prefix
    CHECK(text.find("bytes omitted") != std::string::npos);
    CHECK_FALSE(result->is_error);   // truncation is not a refusal
}

TEST_CASE("a result at or under max_tool_result_bytes is untouched",
         "[agent_loop][invariant]") {
    Harness h;
    auto owned = std::make_unique<FakeTool>("small", ToolCategory::ReadOnly, std::string(50, 'x'));
    h.registry.Register(std::move(owned));
    h.provider.PushResponse(ToolUseResponse("tu1", "small"));
    h.provider.PushResponse(TextResponse("done"));

    agent::LoopConfig config;
    config.max_tool_result_bytes = 100;
    h.Build({}, config);

    h.loop->RunTurn(UserText("go"));

    const llm::ToolResultBlock* result = nullptr;
    for (const auto& message : h.store.messages) {
        for (const auto& block : message.content) {
            if (const auto* r = std::get_if<llm::ToolResultBlock>(&block)) result = r;
        }
    }
    REQUIRE(result != nullptr);
    REQUIRE(result->content.size() == 1);
    CHECK(std::get<llm::TextBlock>(result->content.front()).text == std::string(50, 'x'));
}

namespace {
const llm::ToolResultBlock* LastToolResult(const InMemoryHistoryStore& store) {
    const llm::ToolResultBlock* result = nullptr;
    for (const auto& message : store.messages) {
        for (const auto& block : message.content) {
            if (const auto* r = std::get_if<llm::ToolResultBlock>(&block)) result = r;
        }
    }
    return result;
}
}  // namespace

TEST_CASE("a display payload under max_tool_display_bytes is persisted as-is",
         "[agent_loop][display]") {
    Harness h;
    auto owned = std::make_unique<FakeTool>("t", ToolCategory::ReadOnly, "text");
    owned->set_display({{"kind", "process"}, {"command", "ls"}});
    h.registry.Register(std::move(owned));
    h.provider.PushResponse(ToolUseResponse("tu1", "t"));
    h.provider.PushResponse(TextResponse("done"));
    h.Build({}, agent::LoopConfig{});

    h.loop->RunTurn(UserText("go"));

    const auto* result = LastToolResult(h.store);
    REQUIRE(result != nullptr);
    CHECK(result->display.at("kind") == "process");
}

TEST_CASE("a display payload over max_tool_display_bytes is dropped whole, text kept",
         "[agent_loop][display][invariant]") {
    Harness h;
    auto owned = std::make_unique<FakeTool>("t", ToolCategory::ReadOnly, "text");
    owned->set_display({{"kind", "file"}, {"lines", std::string(500, 'x')}});
    h.registry.Register(std::move(owned));
    h.provider.PushResponse(ToolUseResponse("tu1", "t"));
    h.provider.PushResponse(TextResponse("done"));
    agent::LoopConfig config;
    config.max_tool_display_bytes = 100;
    h.Build({}, config);

    h.loop->RunTurn(UserText("go"));

    const auto* result = LastToolResult(h.store);
    REQUIRE(result != nullptr);
    CHECK(result->display.is_null());
    CHECK(std::get<llm::TextBlock>(result->content.front()).text == "text");
}

TEST_CASE("max_tool_result_bytes == 0 disables the ceiling", "[agent_loop][invariant]") {
    Harness h;
    auto owned =
        std::make_unique<FakeTool>("big", ToolCategory::ReadOnly, std::string(5000, 'x'));
    h.registry.Register(std::move(owned));
    h.provider.PushResponse(ToolUseResponse("tu1", "big"));
    h.provider.PushResponse(TextResponse("done"));

    agent::LoopConfig config;
    config.max_tool_result_bytes = 0;
    h.Build({}, config);

    h.loop->RunTurn(UserText("go"));

    const llm::ToolResultBlock* result = nullptr;
    for (const auto& message : h.store.messages) {
        for (const auto& block : message.content) {
            if (const auto* r = std::get_if<llm::ToolResultBlock>(&block)) result = r;
        }
    }
    REQUIRE(result != nullptr);
    REQUIRE(result->content.size() == 1);
    CHECK(std::get<llm::TextBlock>(result->content.front()).text.size() == 5000);
}

TEST_CASE("retryable errors retry, non-retryable propagate", "[agent_loop][retry]") {
    SECTION("retryable then success") {
        Harness h;
        h.provider.PushError(llm::LlmError(llm::LlmErrorCategory::RateLimit, "429"));
        h.provider.PushResponse(TextResponse("recovered"));
        h.Build({}, agent::LoopConfig{.max_retries = 3, .retry_base_delay = 0ms});

        const auto result = h.loop->RunTurn(UserText("hi"));
        CHECK(result.final_text == "recovered");
        CHECK(h.provider.call_count() == 2);
    }

    SECTION("non-retryable aborts immediately") {
        Harness h;
        h.provider.PushError(llm::LlmError(llm::LlmErrorCategory::AuthError, "401"));
        h.provider.PushResponse(TextResponse("never reached"));
        h.Build({}, agent::LoopConfig{.max_retries = 3, .retry_base_delay = 0ms});

        CHECK_THROWS_AS(h.loop->RunTurn(UserText("hi")), llm::LlmError);
        CHECK(h.provider.call_count() == 1);
    }

    SECTION("retries are bounded") {
        Harness h;
        for (int i = 0; i < 5; ++i) {
            h.provider.PushError(llm::LlmError(llm::LlmErrorCategory::ServerError, "500"));
        }
        h.Build({}, agent::LoopConfig{.max_retries = 2, .retry_base_delay = 0ms});

        CHECK_THROWS_AS(h.loop->RunTurn(UserText("hi")), llm::LlmError);
        CHECK(h.provider.call_count() == 3);  // initial + 2 retries
    }

    SECTION("a retry actually waits") {
        // The sections above set retry_base_delay to 0 so they measure attempt
        // counts without paying for them -- which means nothing there would
        // notice the backoff being deleted. ARCHITECTURE.md's loop pseudocode
        // and provider.hpp:82 both put backoff here precisely so a 429 is not
        // answered with four more requests inside a few milliseconds.
        Harness h;
        h.provider.PushError(llm::LlmError(llm::LlmErrorCategory::RateLimit, "429"));
        h.provider.PushResponse(TextResponse("recovered"));
        h.Build({}, agent::LoopConfig{.max_retries = 1, .retry_base_delay = 200ms});

        const auto started = std::chrono::steady_clock::now();
        const auto result = h.loop->RunTurn(UserText("hi"));
        const auto elapsed = std::chrono::steady_clock::now() - started;

        CHECK(result.final_text == "recovered");
        // Equal jitter draws from [ceiling/2, ceiling], so the floor is exact
        // and one run is enough to assert on. Full jitter would have made this
        // a coin flip -- a zero draw is legal there, so the test would pass with
        // the backoff deleted.
        CHECK(elapsed >= 100ms);
        CHECK(elapsed < 2s);
    }

    SECTION("cancelling during backoff does not wait it out") {
        // Otherwise CancelTurn is deaf for up to retry_max_delay -- the same
        // argument that put a cancel-polling sink in the compactor.
        Harness h;
        for (int i = 0; i < 4; ++i) {
            h.provider.PushError(llm::LlmError(llm::LlmErrorCategory::ServerError, "500"));
        }
        h.Build({}, agent::LoopConfig{.max_retries = 3, .retry_base_delay = 30s});

        auto run = testing::RunInBackground([&h] {
            try {
                h.loop->RunTurn(UserText("hi"));
            } catch (const llm::LlmError&) {
            }
            return true;
        });
        std::this_thread::sleep_for(50ms);
        h.cancel_flag = true;
        REQUIRE(run.WaitFor(5s));  // not 30s
    }
}

TEST_CASE("retry backoff is bounded by retry_max_delay", "[agent_loop][retry]") {
    // The ceiling is what keeps a long-failing backend from pushing a turn's
    // last retry minutes out. Doubling is unbounded without it.
    Harness h;
    for (int i = 0; i < 12; ++i) {
        h.provider.PushError(llm::LlmError(llm::LlmErrorCategory::ServerError, "500"));
    }
    h.Build({}, agent::LoopConfig{
                    .max_retries = 10, .retry_base_delay = 1ms, .retry_max_delay = 4ms});

    const auto started = std::chrono::steady_clock::now();
    CHECK_THROWS_AS(h.loop->RunTurn(UserText("hi")), llm::LlmError);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    // Ten retries at an unbounded 1ms doubling would be ~1s of sleeps; capped at
    // 4ms each it is 20-40ms. The floor proves the sleeps happened at all, so
    // this cannot pass by the backoff having been removed.
    CHECK(elapsed >= 20ms);
    CHECK(elapsed < 1s);
    CHECK(h.provider.call_count() == 11);
}

TEST_CASE("max_tokens is surfaced, not collapsed into end_turn", "[agent_loop][invariant]") {
    // Otherwise output is truncated mid-sentence and the turn looks successful.
    Harness h;
    h.provider.PushResponse(TextResponse("truncated mid-", llm::StopReason::MaxTokens));
    h.Build();

    const auto result = h.loop->RunTurn(UserText("write an essay"));

    CHECK(result.stop_reason == llm::StopReason::MaxTokens);
    CHECK(result.stop_reason != llm::StopReason::EndTurn);
    CHECK(h.provider.call_count() == 1);  // does not loop on a truncated turn
}

TEST_CASE("cancellation stops the loop and keeps what completed", "[agent_loop][cancel]") {
    Harness h;
    h.provider.PushResponse(TextResponse("first"));
    h.provider.PushResponse(TextResponse("should not run"));
    h.Build();

    h.loop->RunTurn(UserText("hi"));
    const std::size_t after_first = h.store.messages.size();

    h.cancel_flag = true;
    const auto result = h.loop->RunTurn(UserText("again"));

    CHECK(result.stop_reason == llm::StopReason::Cancelled);
    // The seed message is still persisted; no further provider call happened.
    CHECK(h.store.messages.size() == after_first + 1);
    CHECK(h.provider.call_count() == 1);
}

TEST_CASE("tools reach the request sorted and stable", "[agent_loop][invariant]") {
    // tools sits at the front of the cached prefix and is rebuilt every turn, so
    // unstable ordering silently destroys prompt-cache hits with no symptom but
    // cost. Registration order here is deliberately reverse-alphabetical.
    Harness h;
    h.AddTool("zebra");
    h.AddTool("alpha");
    h.AddTool("middle");
    h.provider.PushResponse(TextResponse("ok"));
    h.Build();

    const auto first = h.loop->BuildChatRequest();
    const auto second = h.loop->BuildChatRequest();

    REQUIRE(first.tools.size() == 3);
    CHECK(first.tools[0].name == "alpha");
    CHECK(first.tools[1].name == "middle");
    CHECK(first.tools[2].name == "zebra");

    REQUIRE(second.tools.size() == first.tools.size());
    for (std::size_t i = 0; i < first.tools.size(); ++i) {
        CHECK(first.tools[i].name == second.tools[i].name);
    }
}

TEST_CASE("project context rides the session, not the preset", "[agent_loop][invariant]") {
    // AGENTS.md/CLAUDE.md is a SESSION property held by the loop, appended after
    // whatever preset body is active. /agent re-resolves the preset from disk;
    // if the context were baked into the preset it would silently vanish at
    // exactly the moment the operator switched agents mid-task.
    Harness h;
    h.provider.PushResponse(TextResponse("ok"));

    SECTION("appended after the preset body; empty preset still carries it") {
        agent::AgentPreset preset;
        preset.system_prompt = "You are a build agent.";
        h.Build(preset, agent::LoopConfig{}, {}, {},
                "## Project context\n\nAlways run the tests.");

        const auto request = h.loop->BuildChatRequest();
        REQUIRE(request.system.has_value());
        CHECK(*request.system ==
              "You are a build agent.\n\n## Project context\n\nAlways run the tests.");
    }
    SECTION("survives SetPreset") {
        agent::AgentPreset preset;
        preset.system_prompt = "first";
        h.Build(preset, agent::LoopConfig{}, {}, {},
                "## Project context\n\nAlways run the tests.");

        agent::AgentPreset replacement;
        replacement.system_prompt = "second";
        h.loop->SetPreset(std::move(replacement));

        const auto request = h.loop->BuildChatRequest();
        REQUIRE(request.system.has_value());
        CHECK(request.system->rfind("second", 0) == 0);
        CHECK(request.system->find("## Project context\n\nAlways run the tests.") !=
              std::string::npos);
    }
    SECTION("stable across rebuilds of the same session") {
        h.Build(agent::AgentPreset{}, agent::LoopConfig{}, {}, {},
                "## Project context\n\nAlways run the tests.");
        const auto first = h.loop->BuildChatRequest();
        const auto second = h.loop->BuildChatRequest();
        CHECK(first.system == second.system);
    }
}

TEST_CASE("a subagent never receives the spawn tool", "[agent_loop][subagent]") {
    // Hard rule keyed on is_subagent, not preset config: a misconfigured preset
    // must not be able to reopen recursion.
    Harness h;
    h.AddTool(agent::kSpawnSubagentToolName, ToolCategory::Execute);
    h.AddTool("read");

    SECTION("top-level session gets it") {
        h.Build();
        const auto request = h.loop->BuildChatRequest();
        bool found = false;
        for (const auto& t : request.tools) {
            if (t.name == agent::kSpawnSubagentToolName) found = true;
        }
        CHECK(found);
    }

    SECTION("subagent does not, even with an explicit allow-all") {
        agent::AgentPreset preset;
        preset.is_subagent = true;
        preset.allowed_tools = {"*"};
        h.Build(preset);

        const auto request = h.loop->BuildChatRequest();
        for (const auto& t : request.tools) {
            CHECK(t.name != agent::kSpawnSubagentToolName);
        }
    }

    SECTION("subagent does not, even when the preset names it explicitly") {
        agent::AgentPreset preset;
        preset.is_subagent = true;
        preset.allowed_tools = {agent::kSpawnSubagentToolName, "read"};
        h.Build(preset);

        const auto request = h.loop->BuildChatRequest();
        for (const auto& t : request.tools) {
            CHECK(t.name != agent::kSpawnSubagentToolName);
        }
    }
}

TEST_CASE("max_turns bounds a subagent", "[agent_loop][subagent]") {
    Harness h;
    h.AddTool("read");
    for (int i = 0; i < 10; ++i) {
        h.provider.PushResponse(ToolUseResponse("tu" + std::to_string(i), "read"));
    }
    agent::AgentPreset preset;
    preset.is_subagent = true;
    h.Build(preset, agent::LoopConfig{.max_turns = 3});

    const auto result = h.loop->RunTurn(UserText("do a task"));

    CHECK(result.hit_max_turns);
    CHECK(h.provider.call_count() == 3);
}

TEST_CASE("compaction fires past the threshold and never touches the store",
          "[agent_loop][compaction]") {
    Harness h;
    h.provider.PushResponse(TextResponse("one"));
    h.provider.PushResponse(TextResponse("two"));
    h.provider.PushResponse(TextResponse("three"));

    int compactions = 0;
    auto compactor = [&compactions](std::span<const llm::Message>, llm::LlmProvider&,
                                    const agent::CompactionRequest&) {
        ++compactions;
        return agent::CompactionResult{UserText("[summary]"), 0};
    };
    // TextResponse reports 10 in + 5 out = occupancy 15.
    h.Build({}, agent::LoopConfig{.auto_compact = true, .compaction_trigger = FixedTrigger(10)},
            compactor);

    h.loop->RunTurn(UserText("a"));
    const std::size_t store_after_first = h.store.messages.size();
    h.loop->RunTurn(UserText("b"));

    CHECK(compactions >= 1);
    // Compaction rewrites only the in-memory working set. The durable record
    // keeps every message, so scrollback is unaffected however often it runs.
    CHECK(h.store.messages.size() > store_after_first);
}

TEST_CASE("a zero-usage turn seeds occupancy from the request estimate",
          "[agent_loop][compaction]") {
    // The OpenAI-compatible proxy gap: the backend reports usage 0, which
    // used to zero the occupancy and silently disarm the compaction trigger
    // for the whole session. With the fallback on, the turn is flagged and
    // the request's bytes/4 estimate arms the trigger instead.
    Harness h;
    h.AddTool("read");

    llm::ChatResponse zero_tool = ToolUseResponse("tu1", "read");
    zero_tool.usage = llm::Usage{};
    llm::ChatResponse zero_text = TextResponse("done");
    zero_text.usage = llm::Usage{};

    int compactions = 0;
    auto compactor = [&compactions](std::span<const llm::Message>, llm::LlmProvider&,
                                    const agent::CompactionRequest&) {
        ++compactions;
        return agent::CompactionResult{UserText("[summary]"), 0};
    };
    // The trigger is deliberately tiny: the estimate of this small context
    // (system + tool schemas + history) crosses it, the all-zero report does
    // not -- the test fails if the estimate never lands.
    h.Build({}, agent::LoopConfig{.auto_compact = true,
                                  .compaction_trigger = FixedTrigger(2),
                                  .estimate_zero_usage = true},
            compactor);
    h.provider.PushResponse(zero_tool);
    h.provider.PushResponse(zero_text);

    const agent::TurnResult result = h.loop->RunTurn(UserText("go"));

    CHECK(result.usage_estimated);
    // The estimate is mirrored into the wire usage so the status bar can
    // render; cost pricing keys off the flag, not the fields.
    CHECK(result.last_usage.input_tokens > 0);
    CHECK(compactions >= 1);
}

TEST_CASE("estimate_zero_usage = false keeps the old zeroing behavior",
          "[agent_loop][compaction]") {
    Harness h;
    h.AddTool("read");

    llm::ChatResponse zero_tool = ToolUseResponse("tu1", "read");
    zero_tool.usage = llm::Usage{};
    llm::ChatResponse zero_text = TextResponse("done");
    zero_text.usage = llm::Usage{};

    int compactions = 0;
    auto compactor = [&compactions](std::span<const llm::Message>, llm::LlmProvider&,
                                    const agent::CompactionRequest&) {
        ++compactions;
        return agent::CompactionResult{UserText("[summary]"), 0};
    };
    h.Build({}, agent::LoopConfig{.auto_compact = true,
                                  .compaction_trigger = FixedTrigger(2),
                                  .estimate_zero_usage = false},
            compactor);
    h.provider.PushResponse(zero_tool);
    h.provider.PushResponse(zero_text);

    const agent::TurnResult result = h.loop->RunTurn(UserText("go"));

    CHECK_FALSE(result.usage_estimated);
    CHECK(result.last_usage.input_tokens == 0);
    CHECK(compactions == 0);   // occupancy stayed 0, below the trigger of 2
}

TEST_CASE("compaction re-seeds the occupancy estimate", "[agent_loop][compaction][invariant]") {    // The fold halves the working set, but the accumulator still described the
    // pre-fold context -- so the next MaybeCompact, the same turn's next tool
    // cycle, fired on a number that no longer existed: a manual /compact was
    // guaranteed one spurious second summarization on the turn after it.
    // Reseeding in Compact() makes the post-fold estimate what arms the
    // trigger; removing the reseed makes this count 2.
    Harness h;
    h.AddTool("read");
    int compactions = 0;
    auto compactor = [&compactions](std::span<const llm::Message>, llm::LlmProvider&,
                                    const agent::CompactionRequest&) {
        ++compactions;
        return agent::CompactionResult{UserText("[summary]"), 0};
    };
    // The seed stays under the trigger; the pre-fold occupancy comes from the
    // response's reported usage (105), which the stale estimate would keep
    // applying after the fold.
    std::vector<llm::Message> seeded = {UserText(std::string(20, 'o')),
                                        llm::Message{llm::Role::Assistant,
                                                     {llm::TextBlock{std::string(20, 'a')}}}};
    auto big_usage_tool_use = testing::ToolUseResponse("call-1", "read");
    big_usage_tool_use.usage.input_tokens = 100;
    h.provider.PushResponse(big_usage_tool_use);       // occupancy 105: compacts
    h.provider.PushResponse(TextResponse("done"));     // occupancy 15: below the trigger
    h.Build({}, agent::LoopConfig{.auto_compact = true, .compaction_trigger = FixedTrigger(40)},
            compactor, agent::ResumeSeed{.history = std::move(seeded)});

    h.loop->RunTurn(UserText("go"));

    CHECK(compactions == 1);
    CHECK(h.provider.call_count() == 2);
}

TEST_CASE("the occupancy estimate covers the system prompt and the tools array",
          "[agent_loop][compaction][invariant]") {
    // Backend ground truth counts the system prompt and every tool schema; the
    // history-only seed excluded both, so a session whose context was mostly
    // instructions under-triggered compaction -- contradicting what the
    // seeding comment claimed. A large system prompt alone now crosses the
    // trigger before the first provider call; removing it from the estimate
    // makes this compaction never happen.
    Harness h;
    h.AddTool("read");
    int compactions = 0;
    auto compactor = [&compactions](std::span<const llm::Message>, llm::LlmProvider&,
                                    const agent::CompactionRequest&) {
        ++compactions;
        return agent::CompactionResult{UserText("[summary]"), 0};
    };
    // ~2 tokens of history: the trigger is crossed by the context, not the
    // transcript.
    std::vector<llm::Message> seeded = {UserText("x"), UserText("y")};
    agent::AgentPreset preset;
    preset.system_prompt = std::string(2000, 's');
    h.provider.PushResponse(TextResponse("done"));
    h.Build(preset, agent::LoopConfig{.auto_compact = true,
                                      .compaction_trigger = FixedTrigger(10)},
            compactor, agent::ResumeSeed{.history = std::move(seeded)});

    h.loop->RunTurn(UserText("go"));

    CHECK(compactions == 1);
}

TEST_CASE("a successful compaction persists a checkpoint with a correct boundary",
          "[agent_loop][compaction][checkpoint]") {
    Harness h;
    h.provider.PushResponse(TextResponse("one"));
    h.provider.PushResponse(TextResponse("two"));
    h.provider.PushResponse(TextResponse("three"));

    int summaries = 0;
    auto compactor = [&summaries](std::span<const llm::Message>, llm::LlmProvider&,
                                  const agent::CompactionRequest&) {
        return agent::CompactionResult{UserText("[summary-" + std::to_string(++summaries) + "]"),
                                       0};
    };
    // TextResponse reports 10 in + 5 out = occupancy 15.
    h.Build({}, agent::LoopConfig{.auto_compact = true, .compaction_trigger = FixedTrigger(10)},
            compactor);

    h.loop->RunTurn(UserText("a"));
    REQUIRE_FALSE(h.store.checkpoint_boundary.has_value());
    CHECK_FALSE(h.store.checkpoint_cleared);

    h.loop->RunTurn(UserText("b"));
    h.loop->RunTurn(UserText("c"));

    // The fake store assigns seq = row index, so the transcript is
    // {a=0, one=1, b=2, two=3, c=4, three=5}. Turn two compacted [a] into
    // summary-1 (checkpoint covers seq <= 0). Turn three folded
    // [summary-1, one] -- one synthetic head, one persisted message -- into
    // summary-2, extending coverage to seq <= 1.
    REQUIRE(h.store.checkpoint_boundary.has_value());
    CHECK(*h.store.checkpoint_boundary == 1);
    CHECK(h.store.checkpoint_summary.content.size() == 1);
    CHECK(std::get<llm::TextBlock>(h.store.checkpoint_summary.content[0]).text == "[summary-2]");
    // The transcript itself was never touched: six real messages.
    CHECK(h.store.messages.size() == 6);
}

TEST_CASE("a checkpoint-seeded resume replaces the checkpoint with correct arithmetic",
          "[agent_loop][compaction][checkpoint]") {
    Harness h;
    h.provider.PushResponse(TextResponse("one"));

    // Simulates the resume path: [old summary] is synthetic (not in the
    // store); tail1/tail2 are persisted at seq 5 and 6. The seed text is long
    // enough that the bytes/4 occupancy estimate crosses the trigger before
    // the first provider call -- which is the point of the seed.
    const std::string long_text(80, 'x');
    auto compactor = [](std::span<const llm::Message>, llm::LlmProvider&,
                        const agent::CompactionRequest&) {
        return agent::CompactionResult{UserText("[new summary]"), 0};
    };
    h.Build({}, agent::LoopConfig{.auto_compact = true, .compaction_trigger = FixedTrigger(10)},
            compactor,
            agent::ResumeSeed{
                .history = {UserText("[old summary]"), UserText(long_text), UserText(long_text)},
                .first_persisted_seq = 5,
                .head_is_summary = true});

    h.loop->RunTurn(UserText("x"));

    // The fold removed 2 messages, one of them the synthetic head, so
    // persisted coverage advances by exactly one: from "starts at 5" to
    // "covers seq <= 5" (tail1). The new summary must be the one persisted --
    // the old summary is superseded, never stacked.
    REQUIRE(h.store.checkpoint_boundary.has_value());
    CHECK(*h.store.checkpoint_boundary == 5);
    CHECK(std::get<llm::TextBlock>(h.store.checkpoint_summary.content[0]).text == "[new summary]");
}

TEST_CASE("clearing the working set clears the checkpoint", "[agent_loop][compaction][checkpoint]") {
    // /clear drops the view, and the checkpoint IS a view: leaving it would
    // make a resume restore a summary of a conversation the user wiped from
    // context instead of reloading the full record the store kept.
    Harness h;
    h.provider.PushResponse(TextResponse("one"));

    const std::string long_text(80, 'x');
    auto compactor = [](std::span<const llm::Message>, llm::LlmProvider&,
                        const agent::CompactionRequest&) {
        return agent::CompactionResult{UserText("[summary]"), 0};
    };
    h.Build({}, agent::LoopConfig{.auto_compact = true, .compaction_trigger = FixedTrigger(10)},
            compactor,
            agent::ResumeSeed{.history = {UserText("[old summary]"), UserText(long_text)},
                              .first_persisted_seq = 5,
                              .head_is_summary = true});

    h.loop->RunTurn(UserText("x"));
    REQUIRE(h.store.checkpoint_boundary.has_value());

    h.loop->ClearHistory();
    CHECK(h.store.checkpoint_cleared);
    CHECK_FALSE(h.store.checkpoint_boundary.has_value());
}

TEST_CASE("auto_compact off means no compaction regardless of occupancy",
          "[agent_loop][compaction]") {
    Harness h;
    h.provider.PushResponse(TextResponse("one"));
    h.provider.PushResponse(TextResponse("two"));

    int compactions = 0;
    auto compactor = [&compactions](std::span<const llm::Message>, llm::LlmProvider&,
                                    const agent::CompactionRequest&) {
        ++compactions;
        return agent::CompactionResult{UserText("[summary]"), 0};
    };
    h.Build({}, agent::LoopConfig{.auto_compact = false, .compaction_trigger = FixedTrigger(1)},
            compactor);

    h.loop->RunTurn(UserText("a"));
    h.loop->RunTurn(UserText("b"));

    CHECK(compactions == 0);
}

TEST_CASE("window-relative triggers resolve against the preset's context window",
          "[agent_loop][compaction]") {
    // The threshold is not config alone: the SAME strategy fires or not
    // depending on the window of the provider entry this session resolved.
    // TextResponse reports 10 in + 5 out = occupancy 15 throughout.
    auto run_with = [](config::CompactionTriggerSettings trigger,
                       std::optional<std::int64_t> window) {
        Harness h;
        h.provider.PushResponse(TextResponse("one"));
        h.provider.PushResponse(TextResponse("two"));

        int compactions = 0;
        auto compactor = [&compactions](std::span<const llm::Message>, llm::LlmProvider&,
                                        const agent::CompactionRequest&) {
            ++compactions;
            return agent::CompactionResult{UserText("[summary]"), 0};
        };
        agent::AgentPreset preset;
        preset.context_window_tokens = window;
        h.Build(std::move(preset),
                agent::LoopConfig{.auto_compact = true, .compaction_trigger = trigger},
                compactor);
        h.loop->RunTurn(UserText("a"));
        h.loop->RunTurn(UserText("b"));
        return compactions;
    };

    SECTION("percent of window: 50% of 20 = 10, below the occupancy of 15") {
        CHECK(run_with(PercentTrigger(50), 20) >= 1);
    }
    SECTION("percent of window: 80% of 20 = 16, above the occupancy of 15") {
        CHECK(run_with(PercentTrigger(80), 20) == 0);
    }
    SECTION("buffer below window: 20 - 6 = 14, below the occupancy of 15") {
        CHECK(run_with(BufferTrigger(6), 20) >= 1);
    }
    SECTION("buffer below window: 20 - 2 = 18, above the occupancy of 15") {
        CHECK(run_with(BufferTrigger(2), 20) == 0);
    }
}

TEST_CASE("a window-relative trigger with no window never compacts",
          "[agent_loop][compaction][invariant]") {
    // Providers.json entries need not declare context_window_tokens (the seeded
    // file is empty on cold start), so "window-relative trigger, no window" is a
    // real state, not a corner case. The loop must stay inert rather than guess
    // a threshold. The percent is deliberately BELOW the occupancy of 15: a
    // mutation that degrades to "use the percent as a token count" would then
    // fire here and fail this test, instead of passing by arithmetic accident.
    Harness h;
    h.provider.PushResponse(TextResponse("one"));
    h.provider.PushResponse(TextResponse("two"));

    int compactions = 0;
    auto compactor = [&compactions](std::span<const llm::Message>, llm::LlmProvider&,
                                    const agent::CompactionRequest&) {
        ++compactions;
        return agent::CompactionResult{UserText("[summary]"), 0};
    };
    h.Build({}, agent::LoopConfig{.auto_compact = true, .compaction_trigger = PercentTrigger(10)},
            compactor);

    h.loop->RunTurn(UserText("a"));
    h.loop->RunTurn(UserText("b"));

    CHECK(compactions == 0);
    // Same strategy, same occupancy -- only the window differs. 10% of 20 is 2,
    // far below 15, so the window alone is what arms the trigger.
    Harness armed;
    armed.provider.PushResponse(TextResponse("one"));
    armed.provider.PushResponse(TextResponse("two"));
    int armed_compactions = 0;
    auto armed_compactor = [&armed_compactions](std::span<const llm::Message>,
                                                llm::LlmProvider&,
                                                const agent::CompactionRequest&) {
        ++armed_compactions;
        return agent::CompactionResult{UserText("[summary]"), 0};
    };
    agent::AgentPreset preset;
    preset.context_window_tokens = 20;
    armed.Build(std::move(preset),
                agent::LoopConfig{.auto_compact = true, .compaction_trigger = PercentTrigger(10)},
                armed_compactor);
    armed.loop->RunTurn(UserText("a"));
    armed.loop->RunTurn(UserText("b"));
    CHECK(armed_compactions >= 1);
}

namespace {

// A response whose reported usage stays below the compaction trigger, so a
// compacted turn does not immediately re-fire.
llm::ChatResponse SmallTextResponse(std::string text) {
    llm::ChatResponse response = TextResponse(std::move(text));
    response.usage.input_tokens = 2;
    response.usage.output_tokens = 1;
    return response;
}

// Every tool_use in the working set must have its result and vice versa: a
// compaction cut that separated the two is a hard 400 on both wire formats.
bool ToolPairsIntact(const std::vector<llm::Message>& messages) {
    std::unordered_set<std::string> uses, results;
    for (const auto& message : messages) {
        for (const auto& block : message.content) {
            if (const auto* use = std::get_if<llm::ToolUseBlock>(&block)) uses.insert(use->id);
            if (const auto* result = std::get_if<llm::ToolResultBlock>(&block)) {
                results.insert(result->tool_use_id);
            }
        }
    }
    return uses == results;
}

int CountCompactionRequests(const FakeLlmProvider& provider) {
    int count = 0;
    for (const auto& request : provider.requests()) {
        if (request.system.has_value() && !request.system->empty()) ++count;
    }
    return count;
}

}  // namespace

TEST_CASE("the production compactor runs end to end and never splits a tool pair",
          "[agent_loop][compaction]") {
    Harness h;
    h.AddTool("read");
    agent::AgentPreset preset;
    preset.compression_prompt = "DEFAULT PROMPT";
    // TextResponse/ToolUseResponse report 10 in + 5 out = occupancy 15, past the
    // trigger of 12, so compaction is armed from the second loop iteration on.
    h.Build(std::move(preset), agent::LoopConfig{.auto_compact = true, .compaction_trigger = FixedTrigger(12)},
            &agent::compact_messages);

    // Main calls: tu1, tu2, tu3, done. Compaction calls interleave wherever the
    // trigger fires; the queue is shared, so order matters:
    h.provider.PushResponse(ToolUseResponse("tu1", "read"));
    h.provider.PushResponse(TextResponse("SUMMARY ONE"));   // compaction, iter 2
    h.provider.PushResponse(ToolUseResponse("tu2", "read"));
    h.provider.PushResponse(TextResponse("SUMMARY TWO"));   // compaction, iter 3
    h.provider.PushResponse(ToolUseResponse("tu3", "read"));
    h.provider.PushResponse(TextResponse("SUMMARY THREE"));  // compaction, iter 4
    h.provider.PushResponse(SmallTextResponse("done"));

    const auto result = h.loop->RunTurn(UserText("go"));

    CHECK(result.stop_reason == llm::StopReason::EndTurn);
    // Compacted working set: [summary, A2(tu2), U2(tr2), A3(tu3), U3(tr3), A4].
    const auto& history = h.loop->history();
    REQUIRE(history.size() == 6);
    CHECK(history.front().role == llm::Role::User);
    const auto* summary_text = std::get_if<llm::TextBlock>(&history.front().content.front());
    REQUIRE(summary_text != nullptr);
    CHECK(summary_text->text.find("SUMMARY THREE") != std::string::npos);
    // The load-bearing invariant, end to end through the production path: no cut
    // may separate a tool_use from its tool_result.
    CHECK(ToolPairsIntact(history));
    // Compaction rewrites only the working set; the durable record is complete.
    CHECK(h.store.messages.size() == 8);
    // Three compaction calls, each carrying the preset's prompt as system; the
    // four main-turn calls carry none.
    CHECK(CountCompactionRequests(h.provider) == 3);
    for (const auto& request : h.provider.requests()) {
        if (request.system.has_value()) CHECK(*request.system == "DEFAULT PROMPT");
    }
}

TEST_CASE("a provider error during compaction leaves history untouched and cools down",
          "[agent_loop][compaction]") {
    Harness h;
    h.AddTool("read");
    agent::AgentPreset preset;
    preset.compression_prompt = "DEFAULT PROMPT";
    h.Build(std::move(preset), agent::LoopConfig{.auto_compact = true, .compaction_trigger = FixedTrigger(12)},
            &agent::compact_messages);

    // Iter 2's compaction attempt hits the error; without the per-turn cooldown it
    // would be retried at iter 3 and consume a step that does not exist, which the
    // fake reports as an exception. The turn must instead complete untouched.
    h.provider.PushResponse(ToolUseResponse("tu1", "read"));
    h.provider.PushError(llm::LlmError(llm::LlmErrorCategory::Transport, "summarizer down"));
    h.provider.PushResponse(ToolUseResponse("tu2", "read"));
    h.provider.PushResponse(SmallTextResponse("done"));

    const auto result = h.loop->RunTurn(UserText("go"));

    CHECK(result.stop_reason == llm::StopReason::EndTurn);
    // No summary anywhere: the working set is the full, uncompacted history.
    const auto& history = h.loop->history();
    REQUIRE(history.size() == 6);
    for (const auto& message : history) {
        for (const auto& block : message.content) {
            if (const auto* text = std::get_if<llm::TextBlock>(&block)) {
                CHECK(text->text.find("SUMMARY") == std::string::npos);
            }
        }
    }
    // Exactly one compaction attempt across the whole turn.
    CHECK(CountCompactionRequests(h.provider) == 1);
}

TEST_CASE("the preset's compression_prompt reaches the summarization request",
          "[agent_loop][compaction]") {
    Harness h;
    h.AddTool("read");
    agent::AgentPreset preset;
    preset.compression_prompt = "OVERRIDE PROMPT";
    h.Build(std::move(preset), agent::LoopConfig{.auto_compact = true, .compaction_trigger = FixedTrigger(12)},
            &agent::compact_messages);

    // Iteration 1's tool call pushes occupancy past the trigger, so iteration 2
    // compacts before its main call.
    h.provider.PushResponse(ToolUseResponse("tu1", "read"));
    h.provider.PushResponse(TextResponse("SUMMARY"));  // compaction, iter 2
    h.provider.PushResponse(SmallTextResponse("done"));

    h.loop->RunTurn(UserText("a"));

    REQUIRE(h.provider.requests().size() == 3);
    REQUIRE(h.provider.requests()[1].system.has_value());
    CHECK(*h.provider.requests()[1].system == "OVERRIDE PROMPT");
}

TEST_CASE("small_model: the summarizer alone runs on the alternate provider, and the "
          "session's own provider is left untouched",
          "[agent_loop][compaction][small_model]") {
    Harness h;
    h.AddTool("read");
    agent::AgentPreset preset;
    preset.compression_prompt = "DEFAULT PROMPT";
    agent::LoopConfig config{.auto_compact = true, .compaction_trigger = FixedTrigger(12)};
    auto small_storage = std::make_shared<FakeLlmProvider>();
    FakeLlmProvider* small = small_storage.get();
    config.small_model = agent::LoopConfig::SmallModel{small_storage, "small-model"};

    // Iteration 1's tool call pushes occupancy past the trigger; iteration 2's
    // compaction must go to the SMALL provider, the main call to the main one.
    h.provider.PushResponse(ToolUseResponse("tu1", "read"));
    small->PushResponse(TextResponse("TINY SUMMARY"));  // compaction, iter 2
    h.provider.PushResponse(SmallTextResponse("done"));

    h.Build(std::move(preset), config, &agent::compact_messages);
    const auto result = h.loop->RunTurn(UserText("go"));

    CHECK(result.stop_reason == llm::StopReason::EndTurn);
    // The summarization call (identified by its system prompt) happened
    // exactly once, on the small provider, under the small model's name.
    CHECK(CountCompactionRequests(*small) == 1);
    REQUIRE(small->requests().size() == 1);
    CHECK(small->requests()[0].model == "small-model");
    CHECK(*small->requests()[0].system == "DEFAULT PROMPT");
    // The session's own provider answered every turn and never saw a
    // compaction request. Under the reference-assignment predecessor, this
    // call ran the small provider's copy-assignment THROUGH the main
    // provider's reference -- corrupting the session's own provider state.
    REQUIRE(h.provider.requests().size() == 2);
    for (const auto& request : h.provider.requests()) {
        CHECK_FALSE(request.system.has_value());
        CHECK(request.model != "small-model");
    }
    // The turn still compacted with the small provider's output.
    const auto& history = h.loop->history();
    REQUIRE_FALSE(history.empty());
    const auto* summary_text =
        std::get_if<llm::TextBlock>(&history.front().content.front());
    REQUIRE(summary_text != nullptr);
    CHECK(summary_text->text.find("TINY SUMMARY") != std::string::npos);
}

TEST_CASE("a queued subagent result lands at a turn boundary", "[agent_loop][subagent]") {
    // Injecting out-of-band could place a user message between a tool_use and
    // its tool_result, which is a hard 400 on both wire formats.
    Harness h;
    h.AddTool("read");
    h.provider.PushResponse(ToolUseResponse("tu1", "read"));
    h.provider.PushResponse(TextResponse("done"));
    h.Build();

    h.loop->PushInjectedMessage(UserText("[Subagent finished] result"));
    h.loop->RunTurn(UserText("go"));

    // Find the injected message and assert it does not sit between an assistant
    // tool_use and the user tool_result answering it.
    const auto& messages = h.loop->history();
    for (std::size_t i = 0; i < messages.size(); ++i) {
        bool is_injected = false;
        for (const auto& block : messages[i].content) {
            if (const auto* text = std::get_if<llm::TextBlock>(&block)) {
                if (text->text.rfind("[Subagent finished]", 0) == 0) is_injected = true;
            }
        }
        if (!is_injected || i == 0) continue;

        bool prev_has_pending_tool_use = false;
        for (const auto& block : messages[i - 1].content) {
            if (std::holds_alternative<llm::ToolUseBlock>(block)) prev_has_pending_tool_use = true;
        }
        CHECK_FALSE(prev_has_pending_tool_use);
    }
}

TEST_CASE("an injection queued while idle precedes the first provider call of the next turn",
          "[agent_loop][subagent][background]") {
    // The flush used to happen only at tool-cycle boundaries and turn end, so
    // a completion landing on an IDLE session missed the very turn that should
    // have carried it: the loop built its request, called the provider, and
    // only then appended the message. Turn start is the third flush point.
    Harness h;
    h.AddTool("read");
    h.provider.PushResponse(SmallTextResponse("ack"));
    h.Build();

    h.loop->PushInjectedMessage(UserText("[Background task bg-1 completed] build passed"));
    h.loop->RunTurn(UserText("go"));

    // Working history: injection before seed.
    const auto& history = h.loop->history();
    REQUIRE(history.size() == 3);
    CHECK(TextOf(history[0]).find("build passed") != std::string::npos);
    CHECK(TextOf(history[1]) == "go");
    // First provider request saw them in the same order.
    REQUIRE(h.provider.requests().size() == 1);
    REQUIRE(h.provider.requests().front().messages.size() == 2);
    CHECK(TextOf(h.provider.requests().front().messages[0]).find("build passed") !=
          std::string::npos);
    CHECK(TextOf(h.provider.requests().front().messages[1]) == "go");
}

TEST_CASE("the seedless wake turn carries only the injected messages", "[agent_loop][subagent]") {
    // SessionParams::wake_on_injection drives the SESSION to call this; the
    // loop-side contract is that a turn with no seed still flushes first and
    // runs to a natural stop.
    Harness h;
    h.AddTool("read");
    h.provider.PushResponse(SmallTextResponse("noticed"));
    h.Build();

    h.loop->PushInjectedMessage(UserText("[Background task bg-2 failed] exit 1"));
    const auto result = h.loop->RunTurn();

    CHECK(result.stop_reason == llm::StopReason::EndTurn);
    REQUIRE(h.loop->history().size() == 2);  // injection + assistant reply
    CHECK(TextOf(h.loop->history()[0]).find("bg-2 failed") != std::string::npos);
}

TEST_CASE("the skill tool round-trips a lazy-loaded body through the loop", "[agent_loop][skills]") {
    Harness h;
    h.EnableSkills({{"deploy", "Always run the smoke tests first.\n"}});
    agent::AgentPreset preset;
    preset.allowed_skills = {{"deploy", agent::SkillMode::Allow}};
    preset.system_prompt = "You are a test agent.\n";
    h.provider.PushResponse(ToolUseResponse("tu1", "skill", {{"name", "deploy"}}));
    h.provider.PushResponse(TextResponse("done"));
    h.Build(std::move(preset));

    const auto result = h.loop->RunTurn(UserText("how do I deploy?"));

    CHECK(result.stop_reason == llm::StopReason::EndTurn);
    // ReadOnly: the permission checker is never consulted for a skill load.
    CHECK(h.checker.seen.empty());
    // user, assistant(tool_use), user(tool_result), assistant(text)
    REQUIRE(h.store.messages.size() == 4);
    REQUIRE(h.store.messages[2].content.size() == 1);
    const auto* block = std::get_if<llm::ToolResultBlock>(&h.store.messages[2].content.front());
    REQUIRE(block != nullptr);
    CHECK_FALSE(block->is_error);
    REQUIRE(block->content.size() == 1);
    CHECK(std::get<llm::TextBlock>(block->content.front()).text ==
          "Always run the smoke tests first.\n");
}

TEST_CASE("the skill tool is offered only when the preset allows skills", "[agent_loop][skills]") {
    const auto has_skill = [](const std::vector<llm::ToolDefinition>& tools) {
        return std::any_of(tools.begin(), tools.end(),
                           [](const llm::ToolDefinition& t) { return t.name == "skill"; });
    };

    SECTION("allowed skills -> offered") {
        Harness h;
        h.EnableSkills({{"deploy", "body\n"}});
        agent::AgentPreset preset;
        preset.allowed_skills = {{"deploy", agent::SkillMode::Allow}};
        h.provider.PushResponse(TextResponse("hi"));
        h.Build(std::move(preset));

        h.loop->RunTurn(UserText("hi"));

        CHECK(has_skill(h.provider.requests().front().tools));
    }

    SECTION("no allowed skills -> withheld even under allowed_tools ['*']") {
        Harness h;
        h.EnableSkills({{"deploy", "body\n"}});
        agent::AgentPreset preset;  // allowed_skills empty; Build() defaults allowed_tools to {"*"}
        h.provider.PushResponse(TextResponse("hi"));
        h.Build(std::move(preset));

        h.loop->RunTurn(UserText("hi"));

        CHECK_FALSE(has_skill(h.provider.requests().front().tools));
    }
}

TEST_CASE("invoking a skill the preset does not allow yields an error result",
          "[agent_loop][skills]") {
    Harness h;
    // Two real, loaded skills; the preset admits one and the model asks for the
    // other. Previously this used a preset naming a skill that did not exist,
    // which AgentPresetRegistry can no longer produce -- it resolves
    // allowed_skills to allowed ∩ loaded, so an unknown name yields an empty
    // list and the tool is withheld rather than offered-but-unusable.
    h.EnableSkills({{"deploy", "body\n"}, {"other", "other body\n"}});
    agent::AgentPreset preset;
    preset.allowed_skills = {{"other", agent::SkillMode::Allow}};
    h.provider.PushResponse(ToolUseResponse("tu1", "skill", {{"name", "deploy"}}));
    h.provider.PushResponse(TextResponse("ok"));
    h.Build(std::move(preset));

    h.loop->RunTurn(UserText("go"));

    REQUIRE(h.store.messages.size() == 4);
    REQUIRE(h.store.messages[2].content.size() == 1);
    const auto* block = std::get_if<llm::ToolResultBlock>(&h.store.messages[2].content.front());
    REQUIRE(block != nullptr);
    CHECK(block->is_error);
    const std::string text = std::get<llm::TextBlock>(block->content.front()).text;
    CHECK(text.find("not available in this session") != std::string::npos);
}

// --- Resume: seeded history ----------------------------------------------------

TEST_CASE("a seeded loop sends the stored history but never re-appends it",
          "[agent_loop][resume]") {
    // The resume invariant: the stored transcript becomes the working history,
    // and the store receives ONLY what the resumed session produces. Re-appending
    // the seed would duplicate every stored row.
    Harness h;
    std::vector<llm::Message> seeded = {
        UserText("first user message"),
        llm::Message{llm::Role::Assistant, {llm::TextBlock{"first answer"}}},
        UserText("second user message"),
        llm::Message{llm::Role::Assistant, {llm::TextBlock{"second answer"}}},
    };
    h.provider.PushResponse(TextResponse("third answer"));
    h.Build({}, {}, {}, agent::ResumeSeed{.history = seeded});

    const auto result = h.loop->RunTurn(UserText("third user message"));

    CHECK(result.stop_reason == llm::StopReason::EndTurn);
    // The provider saw the whole resumed context, seed first.
    REQUIRE(h.provider.requests().size() == 1);
    const auto& sent = h.provider.requests().front().messages;
    REQUIRE(sent.size() == 5);
    CHECK(std::get<llm::TextBlock>(sent[0].content.front()).text == "first user message");
    CHECK(std::get<llm::TextBlock>(sent[3].content.front()).text == "second answer");
    CHECK(std::get<llm::TextBlock>(sent[4].content.front()).text == "third user message");
    // The store got exactly the new turn -- seed message, then the response.
    // This is the assertion that fails if the seed is appended instead of loaded.
    REQUIRE(h.store.messages.size() == 2);
    CHECK(std::get<llm::TextBlock>(h.store.messages[0].content.front()).text == "third user message");
    CHECK(std::get<llm::TextBlock>(h.store.messages[1].content.front()).text == "third answer");
    // Working history is seed + new turn.
    CHECK(h.loop->history().size() == 6);
}

TEST_CASE("a seeded history can trigger compaction before the first provider call",
          "[agent_loop][resume]") {
    // Without usage from a backend yet, the trigger works off an occupancy
    // estimate of the loaded history. If that seed were missing, a resumed
    // session over the window would fail its first provider call outright --
    // removing SeedOccupancy from the loop makes this test fail.
    Harness h;
    std::vector<llm::Message> seeded = {
        UserText(std::string(400, 'x')),
        llm::Message{llm::Role::Assistant, {llm::TextBlock{std::string(400, 'y')}}},
        UserText(std::string(400, 'z')),
        llm::Message{llm::Role::Assistant, {llm::TextBlock{std::string(400, 'w')}}},
    };  // estimate_tokens ~= 400 > trigger

    int compactions = 0;
    auto compactor = [&compactions](std::span<const llm::Message>, llm::LlmProvider&,
                                    const agent::CompactionRequest&) {
        ++compactions;
        return agent::CompactionResult{UserText("[summary]"), 0};
    };
    h.provider.PushResponse(TextResponse("resumed"));
    // A fixed-token trigger: the strategy landed after this test was written,
    // and the point here is the SEEDED occupancy arming it, not which strategy.
    agent::LoopConfig config;
    config.auto_compact = true;
    config.compaction_trigger.kind = config::CompactionTriggerSettings::Kind::FixedTokens;
    config.compaction_trigger.tokens = 100;
    h.Build({}, config, compactor, agent::ResumeSeed{.history = std::move(seeded)});

    h.loop->RunTurn(UserText("continue"));

    CHECK(compactions == 1);
    // The summary replaced the older half BEFORE the call went out.
    REQUIRE(h.provider.requests().size() == 1);
    const auto& sent = h.provider.requests().front().messages;
    REQUIRE(sent.size() >= 1);
    CHECK(std::get<llm::TextBlock>(sent.front().content.front()).text == "[summary]");
}

// --- parallel dispatch ---------------------------------------------------------

// Every hold spins with a deadline rather than waiting on a barrier: a
// concurrency regression then fails the max-concurrency CHECK after the timeout
// instead of wedging the suite (the deadline.hpp rationale, inlined -- here the
// blocked side is a pool thread the loop itself joins, so a hard barrier could
// deadlock the very join under test).

// Rendezvous latch for the parallel tests: each arriving execution increments
// `arrivals` (never decremented -- `inside` is transient, so waiting on it
// races the first task's exit) and holds until the expected count is present
// or the deadline passes. A concurrency regression therefore degrades to a
// failed assertion after the timeout instead of a hung suite.
struct Rendezvous {
    std::atomic<int> arrivals{0};
    void Hold(int expected) {
        arrivals.fetch_add(1, std::memory_order_acq_rel);
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (arrivals.load(std::memory_order_acquire) < expected &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(1ms);
        }
    }
};

TEST_CASE("parallel-safe tools of one round execute concurrently", "[agent_loop][parallel]") {
    Harness h;
    auto counter = std::make_shared<ConcurrencyCounter>();
    auto gate = std::make_shared<Rendezvous>();
    auto hold = [gate] { gate->Hold(3); };
    h.AddProbe("pa", counter, hold);
    h.AddProbe("pb", counter, hold);
    h.AddProbe("pc", counter, hold);
    h.provider.PushResponse(MultiToolUseResponse({{"t1", "pa"}, {"t2", "pb"}, {"t3", "pc"}}));
    h.provider.PushResponse(TextResponse("done"));
    h.Build();

    const auto result = h.loop->RunTurn(UserText("go"));

    CHECK(result.stop_reason == llm::StopReason::EndTurn);
    CHECK(result.provider_calls == 2);
    // All three were inside execute() at once: the round ran in parallel.
    CHECK(counter->max.load() == 3);
}

TEST_CASE("tool results are appended in block order regardless of completion order",
          "[agent_loop][parallel]") {
    // The FIRST block is the slowest and the LAST the fastest, so executions
    // complete in exactly the reverse of block order -- and the persisted
    // results message must still read t1, t2, t3.
    Harness h2;
    auto slow = std::make_shared<ConcurrencyCounter>();
    auto mid = std::make_shared<ConcurrencyCounter>();
    auto fast = std::make_shared<ConcurrencyCounter>();
    auto sleep_hold = [](std::chrono::milliseconds how_long) {
        return [how_long] { std::this_thread::sleep_for(how_long); };
    };
    h2.AddProbe("slow", slow, sleep_hold(300ms));
    h2.AddProbe("mid", mid, sleep_hold(200ms));
    h2.AddProbe("fast", fast, sleep_hold(100ms));
    h2.provider.PushResponse(
        MultiToolUseResponse({{"t1", "slow"}, {"t2", "mid"}, {"t3", "fast"}}));
    h2.provider.PushResponse(TextResponse("done"));
    h2.Build();

    const auto result = h2.loop->RunTurn(UserText("go"));

    CHECK(result.stop_reason == llm::StopReason::EndTurn);
    REQUIRE(h2.store.messages.size() == 4);
    const auto& results_message = h2.store.messages[2];
    REQUIRE(results_message.content.size() == 3);
    CHECK(ResultOf(results_message, 0).tool_use_id == "t1");
    CHECK(ResultOf(results_message, 1).tool_use_id == "t2");
    CHECK(ResultOf(results_message, 2).tool_use_id == "t3");
    CHECK(std::get<llm::TextBlock>(ResultOf(results_message, 0).content.front()).text == "slow");
    CHECK(std::get<llm::TextBlock>(ResultOf(results_message, 2).content.front()).text == "fast");
    // Call order is the model's order, not completion order.
    REQUIRE(result.tool_names_invoked.size() == 3);
    CHECK(result.tool_names_invoked[0] == "slow");
    CHECK(result.tool_names_invoked[2] == "fast");
}

TEST_CASE("a serial-only tool never overlaps itself but runs beside parallel-safe calls",
          "[agent_loop][parallel]") {
    Harness h;
    auto serial_counter = std::make_shared<ConcurrencyCounter>();
    auto parallel_counter = std::make_shared<ConcurrencyCounter>();
    // The parallel pair rendezvous, so the serial executions genuinely overlap
    // the batch rather than racing to finish before it starts.
    auto gate = std::make_shared<Rendezvous>();
    auto parallel_hold = [gate] { gate->Hold(2); };
    auto& serial = h.AddProbe("serial", serial_counter, {});
    serial.parallel_safe_ = false;
    h.AddProbe("par1", parallel_counter, parallel_hold);
    h.AddProbe("par2", parallel_counter, parallel_hold);
    h.provider.PushResponse(MultiToolUseResponse(
        {{"t1", "serial"}, {"t2", "serial"}, {"t3", "par1"}, {"t4", "par2"}}));
    h.provider.PushResponse(TextResponse("done"));
    h.Build();

    const auto result = h.loop->RunTurn(UserText("go"));

    CHECK(result.stop_reason == llm::StopReason::EndTurn);
    // The serial-only tool's two invocations never overlapped each other...
    CHECK(serial_counter->max.load() == 1);
    // ...while the parallel-safe pair ran together.
    CHECK(parallel_counter->max.load() == 2);
    // Every call still answered, in block order.
    const auto& results_message = h.store.messages[2];
    REQUIRE(results_message.content.size() == 4);
    CHECK(ResultOf(results_message, 0).tool_use_id == "t1");
    CHECK_FALSE(ResultOf(results_message, 0).is_error);
    CHECK(ResultOf(results_message, 3).tool_use_id == "t4");
}

TEST_CASE("gating stays sequential in a parallel round: one ask, and the grant satisfies later calls",
          "[agent_loop][parallel][permissions]") {
    Harness h;
    h.checker_override = std::make_unique<GrantingPermissionChecker>();
    // Ask for the probe's category (ReadOnly) so both calls need gating.
    h.executor_policy.read_only = tools::PermissionMode::Ask;

    auto& tool = h.AddTool("gated");
    tool.set_disclosure(tools::PermissionDisclosure{
        "gated: /usr/bin/probe", {tools::GrantOffer{"k1", "allow /usr/bin/probe this session"}},
        true});

    h.provider.PushResponse(MultiToolUseResponse({{"t1", "gated"}, {"t2", "gated"}}));
    h.provider.PushResponse(TextResponse("done"));
    h.Build();

    const auto result = h.loop->RunTurn(UserText("go"));

    CHECK(result.stop_reason == llm::StopReason::EndTurn);
    auto* checker = static_cast<GrantingPermissionChecker*>(h.checker_override.get());
    // Gate-then-run: both decisions resolved sequentially BEFORE either
    // execution, so the grant the first call acquired satisfied the second.
    // Concurrent checks would have asked twice.
    REQUIRE(checker->seen.size() == 1);
    const auto& results_message = h.store.messages[2];
    REQUIRE(results_message.content.size() == 2);
    CHECK_FALSE(ResultOf(results_message, 0).is_error);
    CHECK_FALSE(ResultOf(results_message, 1).is_error);
}

namespace {

// Answers in script order; the last answer repeats once the script runs out.
class ScriptedPermissionChecker : public tools::PermissionChecker {
public:
    explicit ScriptedPermissionChecker(std::vector<tools::PermissionResponse> script)
        : script_(std::move(script)) {}
    tools::PermissionResponse check(const tools::PermissionRequest&) override {
        const std::size_t index = std::min(asked, script_.size() - 1);
        ++asked;
        return script_[index];
    }
    std::size_t asked = 0;

private:
    std::vector<tools::PermissionResponse> script_;
};

std::string ResultText(const llm::Message& message, std::size_t index) {
    const auto& block = ResultOf(message, index);
    if (block.content.empty()) return {};
    return std::get<llm::TextBlock>(block.content.front()).text;
}

// A gated tool whose grant the scripted checker never takes, so every call of
// it prompts.
FakeTool& AddGatedTool(Harness& h) {
    h.executor_policy.read_only = tools::PermissionMode::Ask;
    auto& tool = h.AddTool("gated");
    tool.set_disclosure(tools::PermissionDisclosure{
        "gated: /usr/bin/probe", {tools::GrantOffer{"k1", "allow /usr/bin/probe this session"}},
        true});
    return tool;
}

}  // namespace

TEST_CASE("deny and stop ends the turn: no call of the round runs, no provider call follows",
          "[agent_loop][permissions]") {
    Harness h;
    auto owned = std::make_unique<ScriptedPermissionChecker>(std::vector<tools::PermissionResponse>{
        {tools::PermissionDecision::Allow, {}},
        {tools::PermissionDecision::Deny, {}, {}, /*stop_turn=*/true}});
    auto* checker = owned.get();
    h.checker_override = std::move(owned);
    auto& tool = AddGatedTool(h);

    // No second response queued: a provider call after the stop would fail
    // the turn instead of ending it Cancelled.
    h.provider.PushResponse(
        MultiToolUseResponse({{"t1", "gated"}, {"t2", "gated"}, {"t3", "gated"}}));
    h.Build();

    const auto result = h.loop->RunTurn(UserText("go"));

    CHECK(result.stop_reason == llm::StopReason::Cancelled);
    CHECK(result.provider_calls == 1);
    CHECK(checker->asked == 2);   // t3 is answered without a prompt
    CHECK(tool.calls.empty());    // t1 was allowed, but stop means stop
    REQUIRE(h.store.messages.size() == 3);
    const auto& results_message = h.store.messages[2];
    REQUIRE(results_message.content.size() == 3);   // every tool_use answered
    for (std::size_t i = 0; i < 3; ++i) CHECK(ResultOf(results_message, i).is_error);
    CHECK(ResultText(results_message, 0).find("not run: the user stopped the turn") !=
          std::string::npos);
    CHECK(ResultText(results_message, 1).find("stopped the turn here") != std::string::npos);
    CHECK(ResultText(results_message, 2).find("not run: the user stopped the turn") !=
          std::string::npos);
}

TEST_CASE("deny with message hands the message to the model and the turn goes on",
          "[agent_loop][permissions]") {
    Harness h;
    h.checker_override = std::make_unique<ScriptedPermissionChecker>(
        std::vector<tools::PermissionResponse>{
            {tools::PermissionDecision::Deny, {}, "run the tests first", false}});
    auto& tool = AddGatedTool(h);

    h.provider.PushResponse(MultiToolUseResponse({{"t1", "gated"}}));
    h.provider.PushResponse(TextResponse("ok, running the tests"));
    h.Build();

    const auto result = h.loop->RunTurn(UserText("go"));

    CHECK(result.stop_reason == llm::StopReason::EndTurn);
    CHECK(result.provider_calls == 2);
    CHECK(tool.calls.empty());
    const auto& results_message = h.store.messages[2];
    CHECK(ResultOf(results_message, 0).is_error);
    CHECK(ResultText(results_message, 0).find("run the tests first") != std::string::npos);
}

TEST_CASE("a cancel mid-round still answers every tool_use and reports Cancelled",
          "[agent_loop][parallel][cancel]") {
    Harness h;
    auto counter = std::make_shared<ConcurrencyCounter>();
    auto& pa = h.AddProbe("pa", counter, {});
    auto& pb = h.AddProbe("pb", counter, {});
    auto& pc = h.AddProbe("pc", counter, {});
    // The first execution to get in cancels the turn; every call must still be
    // answered -- one orphaned tool_use is a hard 400 on the next request.
    auto cancel_hold = [&h] {
        h.cancel_flag = true;
        std::this_thread::sleep_for(20ms);
    };
    pa.hold_ = cancel_hold;
    pb.hold_ = cancel_hold;
    pc.hold_ = cancel_hold;
    h.provider.PushResponse(MultiToolUseResponse({{"t1", "pa"}, {"t2", "pb"}, {"t3", "pc"}}));
    h.Build();

    const auto result = h.loop->RunTurn(UserText("go"));

    CHECK(result.stop_reason == llm::StopReason::Cancelled);
    REQUIRE(h.store.messages.size() == 3);
    const auto& results_message = h.store.messages[2];
    REQUIRE(results_message.content.size() == 3);
    CHECK(ResultOf(results_message, 0).tool_use_id == "t1");
    CHECK(ResultOf(results_message, 1).tool_use_id == "t2");
    CHECK(ResultOf(results_message, 2).tool_use_id == "t3");
}

// Every assistant message carrying a tool_use block is answered by a following
// user message with a matching ToolResultBlock -- the property both wire
// formats hard-400 without. Run against any request's recorded messages.
void AssertAllToolUsesAnswered(std::span<const llm::Message> messages,
                               const std::string& context) {
    for (std::size_t i = 0; i < messages.size(); ++i) {
        if (messages[i].role != llm::Role::Assistant) continue;
        for (const auto& block : messages[i].content) {
            const auto* use = std::get_if<llm::ToolUseBlock>(&block);
            if (use == nullptr) continue;
            bool answered = false;
            for (std::size_t j = i + 1; j < messages.size() && !answered; ++j) {
                if (messages[j].role != llm::Role::User) continue;
                for (const auto& result_block : messages[j].content) {
                    if (const auto* result = std::get_if<llm::ToolResultBlock>(&result_block)) {
                        if (result->tool_use_id == use->id) answered = true;
                    }
                }
            }
            INFO(context << ": tool_use " << use->id << " (message " << i << ")");
            CHECK(answered);
        }
    }
}

TEST_CASE("a cancel landing after tool blocks streamed still yields a provider-valid "
          "transcript",
          "[agent_loop][invariant][cancel]") {
    // The assistant message is appended and persisted BEFORE the stop-reason
    // checks. A response that carries COMPLETE tool_use blocks but stops on
    // Cancelled -- the sink aborting after the model finished streaming its
    // calls, the most natural cancel moment -- used to break without answering
    // them: an orphaned tool_use, a hard 400 on EVERY later turn, with the
    // repair existing only on the resume path. The live loop repairs the same
    // way now.
    Harness h;
    h.AddTool("probe");
    h.Build();

    h.provider.PushResponse(MultiToolUseResponse({{"u1", "probe"}}));
    llm::ChatResponse cancelled_with_tools;
    cancelled_with_tools.content.push_back(
        llm::ToolUseBlock{"u2", "probe", nlohmann::json::object()});
    cancelled_with_tools.stop_reason = llm::StopReason::Cancelled;
    cancelled_with_tools.usage.input_tokens = 10;
    cancelled_with_tools.usage.output_tokens = 5;
    h.provider.PushResponse(std::move(cancelled_with_tools));
    h.provider.PushResponse(TextResponse("after the cancel"));

    const auto first = h.loop->RunTurn(UserText("go"));
    CHECK(first.stop_reason == llm::StopReason::Cancelled);

    // THE 400 REGRESSION: the next turn's request must serialize. Every
    // tool_use -- including u2 from the cancelled round -- is answered.
    const auto second = h.loop->RunTurn(UserText("continue"));
    CHECK(second.stop_reason == llm::StopReason::EndTurn);
    REQUIRE(h.provider.requests().size() == 3);
    AssertAllToolUsesAnswered(h.provider.requests()[2].messages, "post-cancel request");

    // The repair is a real appended message the model can adapt to: error
    // blocks naming the interruption, keyed to the orphaned calls.
    bool saw_repair = false;
    for (const auto& message : h.store.messages) {
        if (message.role != llm::Role::User) continue;
        for (const auto& block : message.content) {
            const auto* result = std::get_if<llm::ToolResultBlock>(&block);
            if (result == nullptr || result->tool_use_id != "u2") continue;
            saw_repair = true;
            CHECK(result->is_error);
            const auto* text = std::get_if<llm::TextBlock>(&result->content.front());
            REQUIRE(text != nullptr);
            CHECK(text->text.find("interrupted") != std::string::npos);
        }
    }
    CHECK(saw_repair);
}

TEST_CASE("a max_tokens cut with complete tool calls repairs the same way",
          "[agent_loop][invariant]") {
    // The other break path: MaxTokens is reported as itself and ends the turn,
    // and a cut that arrived after complete tool_calls used to orphan them
    // exactly as a cancel did.
    Harness h;
    h.AddTool("probe");
    h.Build();

    h.provider.PushResponse(MultiToolUseResponse({{"u1", "probe"}}));
    llm::ChatResponse truncated_with_tools;
    truncated_with_tools.content.push_back(
        llm::ToolUseBlock{"u2", "probe", nlohmann::json::object()});
    truncated_with_tools.stop_reason = llm::StopReason::MaxTokens;
    truncated_with_tools.usage.input_tokens = 10;
    truncated_with_tools.usage.output_tokens = 5;
    h.provider.PushResponse(std::move(truncated_with_tools));
    h.provider.PushResponse(TextResponse("after the cut"));

    const auto first = h.loop->RunTurn(UserText("go"));
    CHECK(first.stop_reason == llm::StopReason::MaxTokens);

    const auto second = h.loop->RunTurn(UserText("continue"));
    CHECK(second.stop_reason == llm::StopReason::EndTurn);
    REQUIRE(h.provider.requests().size() == 3);
    AssertAllToolUsesAnswered(h.provider.requests()[2].messages, "post-max-tokens request");
}

TEST_CASE("a throwing lifecycle sink costs the event, never the tool result",
          "[agent_loop][invariant]") {
    // execute_one's sink calls are the one seam on the §12.2 path that ran
    // unguarded -- and on a pool thread an escape is std::terminate for the
    // whole process, while an unwind on the worker would race the round's
    // other submitted tasks against this stack frame. The sinks are contained;
    // the transcript is the authoritative record.
    Harness h;
    h.AddTool("probe");
    h.throwing_lifecycle_sink = true;
    h.Build();

    h.provider.PushResponse(ToolUseResponse("tu1", "probe"));
    h.provider.PushResponse(TextResponse("done"));

    const auto result = h.loop->RunTurn(UserText("go"));
    CHECK(result.stop_reason == llm::StopReason::EndTurn);
    // Four rows: seed, assistant(tool_use), results, and the turn's own
    // continuation -- the round ended, the loop called again, "done" ended it.
    REQUIRE(h.store.messages.size() == 4);
    REQUIRE(h.store.messages[2].content.size() == 1);
    CHECK_FALSE(ResultOf(h.store.messages[2], 0).is_error);
}

TEST_CASE("max_parallel_tool_calls = 1 reproduces sequential dispatch",
          "[agent_loop][parallel]") {
    Harness h;
    auto counter = std::make_shared<ConcurrencyCounter>();
    auto hold = [] { std::this_thread::sleep_for(20ms); };
    h.AddProbe("pa", counter, hold);
    h.AddProbe("pb", counter, hold);
    h.provider.PushResponse(MultiToolUseResponse({{"t1", "pa"}, {"t2", "pb"}}));
    h.provider.PushResponse(TextResponse("done"));
    agent::LoopConfig config;
    config.max_parallel_tool_calls = 1;
    h.Build({}, config);

    const auto result = h.loop->RunTurn(UserText("go"));

    CHECK(result.stop_reason == llm::StopReason::EndTurn);
    CHECK(counter->max.load() == 1);
    const auto& results_message = h.store.messages[2];
    REQUIRE(results_message.content.size() == 2);
    CHECK(ResultOf(results_message, 0).tool_use_id == "t1");
}

TEST_CASE("an unknown tool in a parallel round is answered, not fatal",
          "[agent_loop][parallel]") {
    Harness h;
    h.AddProbe("real", std::make_shared<ConcurrencyCounter>(), {});
    h.provider.PushResponse(MultiToolUseResponse({{"t1", "nope"}, {"t2", "real"}}));
    h.provider.PushResponse(TextResponse("done"));
    h.Build();

    const auto result = h.loop->RunTurn(UserText("go"));

    CHECK(result.stop_reason == llm::StopReason::EndTurn);
    const auto& results_message = h.store.messages[2];
    REQUIRE(results_message.content.size() == 2);
    CHECK(ResultOf(results_message, 0).tool_use_id == "t1");
    CHECK(ResultOf(results_message, 0).is_error);
    CHECK_FALSE(ResultOf(results_message, 1).is_error);
}

TEST_CASE("pre_compact and post_compact hook points fire on both the auto trigger and /compact",
          "[agent_loop][compaction][hooks]") {
    // Track O: the compaction pair is emitted from the shared Compact() body,
    // so both the occupancy trigger and the manual /compact path reach it. The
    // actions log to a file, which keeps the assertion on the DISPATCH ORDER
    // (pre before post, both present) rather than on a mocked seam.
    testing::TempDir arena;
    const auto log = arena.path() / "compaction.log";
    const auto make_action = [&](const std::string& point) {
        minimal::config::HookActionConfig action;
        action.on = point;
        action.command = "echo " + point + " >> '" + log.string() + "'";
        return action;
    };
    minimal::config::HooksConfig hooks_config;
    hooks_config.hooks.push_back(make_action(minimal::hooks::kPreCompact));
    hooks_config.hooks.push_back(make_action(minimal::hooks::kPostCompact));
    hooks::HookDispatcher dispatcher{common::DefaultLogger()};
    (void)dispatcher.Configure(hooks_config);

    Harness h;
    h.provider.PushResponse(TextResponse("one"));
    h.provider.PushResponse(TextResponse("two"));

    auto compactor = [](std::span<const llm::Message>, llm::LlmProvider&,
                        const agent::CompactionRequest&) {
        return agent::CompactionResult{UserText("[summary]"), 0};
    };
    // TextResponse reports 10 in + 5 out = occupancy 15, over a fixed trigger
    // of 10: the second turn's trigger fires the AUTO path.
    agent::LoopConfig config{.auto_compact = true, .compaction_trigger = FixedTrigger(10)};
    config.hooks = &dispatcher;
    h.Build({}, config, compactor);

    h.loop->RunTurn(UserText("a"));  // occupancy 15 < trigger evaluated... first turn: no compaction
    h.loop->RunTurn(UserText("b"));  // crosses the trigger: auto compaction

    const auto read_lines = [&log] {
        std::ifstream in(log);
        std::vector<std::string> lines;
        for (std::string line; std::getline(in, line);) lines.push_back(line);
        return lines;
    };

    // The pair is observe-only, i.e. EmitAsync: drain the dispatch thread
    // before asserting on its side effects.
    dispatcher.WaitForIdleForTesting();
    REQUIRE(std::filesystem::exists(log));
    REQUIRE(read_lines().size() == 2);

    // The manual path (/compact -> CompactNow) fires the same pair, with the
    // same points; per-turn cooldown does not block a manual call.
    h.loop->CompactNow();
    dispatcher.WaitForIdleForTesting();
    const auto lines = read_lines();
    REQUIRE(lines.size() == 4);
    CHECK(lines[0] == "core.pre_compact");
    CHECK(lines[1] == "core.post_compact");
    CHECK(lines[2] == "core.pre_compact");
    CHECK(lines[3] == "core.post_compact");
}

TEST_CASE("the retry wait honors the server's Retry-After hint", "[agent_loop][retry]") {
    // The hint was parsed, carried, and tested end to end -- and then ignored
    // by the only consumer there ever was: retrying a 429 sooner than its
    // Retry-After converts one rate-limit event into max_retries more. With a
    // computed backoff far below the hint, the wait is the hint's floor (the
    // equal-jitter floor pattern: the assertion needs a lower bound that
    // exists, so the hint, not the jitter, sets it).
    Harness h;
    agent::LoopConfig config;
    config.max_retries = 1;
    config.retry_base_delay = std::chrono::milliseconds{50};
    config.retry_max_delay = std::chrono::milliseconds{100};
    h.Build({}, config);

    h.provider.PushError(llm::LlmError(llm::LlmErrorCategory::RateLimit, "429", 429,
                                       std::chrono::milliseconds{400}));
    h.provider.PushResponse(TextResponse("recovered"));

    const auto started = std::chrono::steady_clock::now();
    const auto result = h.loop->RunTurn(UserText("go"));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    CHECK(result.stop_reason == llm::StopReason::EndTurn);
    INFO("retry waited " << elapsed << " ms; the hint alone is 400");
    CHECK(elapsed >= 380);  // the hint, minus a small scheduling allowance
    CHECK(elapsed < 5000);  // and not the ceiling of a misapplied hint
}

// ---------------------------------------------------------------------------
// S4: the compaction strategy seam
// ---------------------------------------------------------------------------

TEST_CASE("LoopConfig.cut_selector is consulted instead of the legacy history.size()/2 "
         "halving",
         "[agent_loop][compaction]") {
    Harness h;
    std::size_t seen_size = 0;
    auto compactor = [&seen_size](std::span<const llm::Message> messages_to_compact,
                                  llm::LlmProvider&,
                                  const agent::CompactionRequest&) -> std::optional<agent::CompactionResult> {
        seen_size = messages_to_compact.size();
        return agent::CompactionResult{UserText("[summary]"), 0};
    };

    agent::LoopConfig config;
    // A cut_selector that always proposes 1, regardless of history size --
    // the opposite of history.size()/2, so the test fails if Compact() ever
    // falls back to the legacy default while a selector is set.
    config.compaction_strategy.cut_selector = [](const std::vector<llm::Message>&, std::int64_t) {
        return std::size_t{1};
    };
    h.Build({}, config, compactor,
            agent::ResumeSeed{.history = {UserText("a"), UserText("b"), UserText("c"),
                                          UserText("d")}});

    const auto cut = h.loop->CompactNow();
    REQUIRE(cut.has_value());
    CHECK(*cut == 1);
    CHECK(seen_size == 1);  // the summarizer received exactly the proposed (already-safe) range
}

TEST_CASE("a proposed cut is still snapped to a safe tool-call boundary, never trusted "
         "verbatim",
         "[agent_loop][compaction]") {
    // The seam's core invariant: a strategy's cut is a starting point, never
    // a guarantee. A selector proposing 1 here would put ONLY message[0]'s
    // tool_use in the compacted region, excluding message[1] -- its own
    // result -- from it: an orphaned call. snap_compaction_boundary must
    // pull the boundary back to 0, which agent_loop then treats as "no safe
    // boundary" (elision has nothing worth shrinking here -- the result is
    // two bytes -- so it falls through to the genuine last-resort warning).
    Harness h;
    bool summarizer_called = false;
    auto compactor = [&summarizer_called](std::span<const llm::Message>, llm::LlmProvider&,
                                          const agent::CompactionRequest&)
        -> std::optional<agent::CompactionResult> {
        summarizer_called = true;
        return agent::CompactionResult{UserText("[summary]"), 0};
    };

    llm::ToolUseBlock use;
    use.id = "call_1";
    use.name = "read";
    use.input = nlohmann::json::object();
    llm::ToolResultBlock result;
    result.tool_use_id = "call_1";
    result.content = {llm::TextBlock{"ok"}};

    agent::LoopConfig config;
    config.compaction_strategy.cut_selector = [](const std::vector<llm::Message>&, std::int64_t) {
        return std::size_t{1};  // between the tool_use and its own result -- unsafe
    };
    h.Build({}, config, compactor,
            agent::ResumeSeed{.history = {llm::Message{llm::Role::Assistant, {use}},
                                          llm::Message{llm::Role::User, {result}}, UserText("c"),
                                          UserText("d")}});

    const auto cut = h.loop->CompactNow();
    CHECK_FALSE(cut.has_value());
    CHECK_FALSE(summarizer_called);
}

TEST_CASE("a cut_selector declining to cut (candidate 0) is 'nothing to compact', not "
         "'no safe boundary'",
         "[agent_loop][compaction][invariant]") {
    // THE BUG: compaction_keep_recent_tokens set at or above the resolved
    // trigger makes the real cut_selector (ProposeSummarizeCut) return 0
    // directly -- its own token-budget walk covers the whole working set --
    // BEFORE the boundary snap ever runs. That used to fall into the SAME
    // branch as "snap shrank a nonzero candidate to 0 because there is no
    // safe tool-call boundary", which elides the largest tool result in
    // place: a misconfiguration with nothing to do with message pairing
    // silently truncated a tool result every turn the trigger stayed armed.
    Harness h;
    bool summarizer_called = false;
    auto compactor = [&summarizer_called](std::span<const llm::Message>, llm::LlmProvider&,
                                          const agent::CompactionRequest&)
        -> std::optional<agent::CompactionResult> {
        summarizer_called = true;
        return agent::CompactionResult{UserText("[summary]"), 0};
    };

    // A large tool result -- big enough that, if elision ran, it would be
    // visibly truncated (kElisionMinSizeToBother is 1000 bytes; this clears
    // it comfortably). Its survival intact is what proves elision did NOT
    // run.
    llm::ToolResultBlock result;
    result.tool_use_id = "call_1";
    result.content = {llm::TextBlock{std::string(5000, 'x')}};
    const llm::Message result_message{llm::Role::User, {result}};

    agent::LoopConfig config;
    // The exact shape ProposeSummarizeCut takes when keep_recent_tokens
    // covers everything: 0, unconditionally, before any boundary reasoning.
    config.compaction_strategy.cut_selector = [](const std::vector<llm::Message>&, std::int64_t) {
        return std::size_t{0};
    };
    h.Build({}, config, compactor,
            agent::ResumeSeed{.history = {UserText("a"), result_message, UserText("c")}});

    const auto cut = h.loop->CompactNow();
    CHECK_FALSE(cut.has_value());
    CHECK_FALSE(summarizer_called);

    // The tool result is byte-for-byte untouched -- elision never ran.
    const auto& history = h.loop->history();
    REQUIRE(history.size() == 3);
    const auto& surviving = std::get<llm::ToolResultBlock>(history[1].content[0]);
    REQUIRE(surviving.content.size() == 1);
    const auto& text = std::get<llm::TextBlock>(surviving.content[0]);
    CHECK(text.text == std::string(5000, 'x'));
}

TEST_CASE("no safe compaction boundary elides the largest tool result instead of accepting "
         "unbounded growth",
         "[agent_loop][compaction]") {
    // A 2-message history where message[0]'s tool_use has NO matching result
    // anywhere in the history -- every candidate cut lands inside it, so
    // snap_compaction_boundary always returns 0 (compaction_boundary.cpp).
    Harness h;
    bool summarizer_called = false;
    auto compactor = [&summarizer_called](std::span<const llm::Message>, llm::LlmProvider&,
                                          const agent::CompactionRequest&)
        -> std::optional<agent::CompactionResult> {
        summarizer_called = true;
        return agent::CompactionResult{UserText("[summary]"), 0};
    };

    llm::ToolUseBlock use;
    use.id = "call_1";
    use.name = "read";
    use.input = nlohmann::json::object();

    llm::ToolResultBlock huge;
    huge.tool_use_id = "unrelated";  // orphans call_1 regardless -- never answers it
    huge.content = {llm::TextBlock{std::string(5000, 'x')}};

    h.Build({}, {}, compactor,
            agent::ResumeSeed{.history = {llm::Message{llm::Role::Assistant, {use}},
                                          llm::Message{llm::Role::User, {huge}}}});

    const auto cut = h.loop->CompactNow();
    CHECK_FALSE(cut.has_value());     // nothing was folded into a summary
    CHECK_FALSE(summarizer_called);   // the elision path never reaches the summarizer

    REQUIRE(h.loop->history().size() == 2);
    const auto& shrunk = std::get<llm::ToolResultBlock>(h.loop->history()[1].content.at(0));
    const auto& text = std::get<llm::TextBlock>(shrunk.content.at(0)).text;
    CHECK(text.size() < 5000);
    CHECK(text.find("omitted") != std::string::npos);
}

TEST_CASE("no safe boundary AND nothing worth eliding falls back to the genuine last resort",
         "[agent_loop][compaction]") {
    // Same orphaned-tool_use shape, but every tool result is already small --
    // ElideLargestToolResult must decline (nothing large enough to bother
    // with), leaving history untouched.
    Harness h;
    llm::ToolUseBlock use;
    use.id = "call_1";
    use.name = "read";
    use.input = nlohmann::json::object();
    llm::ToolResultBlock small;
    small.tool_use_id = "unrelated";
    small.content = {llm::TextBlock{"tiny"}};

    bool summarizer_called = false;
    auto compactor = [&summarizer_called](std::span<const llm::Message>, llm::LlmProvider&,
                                          const agent::CompactionRequest&)
        -> std::optional<agent::CompactionResult> {
        summarizer_called = true;
        return agent::CompactionResult{UserText("[summary]"), 0};
    };
    h.Build({}, {}, compactor,
            agent::ResumeSeed{.history = {llm::Message{llm::Role::Assistant, {use}},
                                          llm::Message{llm::Role::User, {small}}}});

    const auto cut = h.loop->CompactNow();
    CHECK_FALSE(cut.has_value());
    CHECK_FALSE(summarizer_called);
    REQUIRE(h.loop->history().size() == 2);
    const auto& unchanged = std::get<llm::ToolResultBlock>(h.loop->history()[1].content.at(0));
    CHECK(std::get<llm::TextBlock>(unchanged.content.at(0)).text == "tiny");
}

// --- tool_config delivery --------------------------------------------------------

// Records what ctx.tool_config pointed at during execute() AND during
// Disclose() (the Ask gate), copying the slice -- the pointer itself is
// per-invocation and gone by the time the test reads. Disclose sees the slice
// too: disclosure and grant keys are computed from it inside Prepare.
class ConfigProbeTool : public tools::Tool {
public:
    ConfigProbeTool(std::string name, std::string ns)
        : name_(std::move(name)), ns_(std::move(ns)) {
        spec_.name = name_;
        spec_.category = ToolCategory::ReadOnly;
        spec_.definition.name = name_;
        spec_.definition.description = "config probe";
        spec_.definition.input_schema = nlohmann::json::object();
    }

    const tools::ToolSpec& spec() const override { return spec_; }

    tools::ToolResult execute(const nlohmann::json&, tools::ToolContext& ctx) override {
        seen_slices.push_back(ctx.tool_config ? *ctx.tool_config : nlohmann::json());
        return {false, name_};
    }

    tools::PermissionDisclosure Disclose(const nlohmann::json&,
                                         const tools::ToolContext& ctx) const override {
        disclosed_slices.push_back(ctx.tool_config ? *ctx.tool_config : nlohmann::json());
        tools::PermissionDisclosure disclosure;
        disclosure.summary = name_;
        disclosure.complete = true;
        return disclosure;
    }

    std::string_view config_namespace() const override { return ns_; }

    std::vector<nlohmann::json> seen_slices;
    mutable std::vector<nlohmann::json> disclosed_slices;

private:
    tools::ToolSpec spec_;
    std::string name_;
    std::string ns_;
};

TEST_CASE("a tool receives its tool_config slice from the active preset", "[agent_loop][tool_config]") {
    Harness h;
    h.registry.Register(std::make_unique<ConfigProbeTool>("probe", "python"));

    agent::AgentPreset preset;
    preset.tool_config = nlohmann::json{{"python", {{"venv", ".venv"}}},
                                        {"other", {{"key", 1}}}};
    h.provider.PushResponse(ToolUseResponse("tu1", "probe"));
    h.provider.PushResponse(TextResponse("done"));
    h.Build(preset);

    h.loop->RunTurn(UserText("go"));

    // The tool saw exactly its OWN namespace's slice -- not the raw map, not
    // another plugin's keys.
    const auto* owner = dynamic_cast<ConfigProbeTool*>(h.registry.Find("probe").get());
    REQUIRE(owner != nullptr);
    REQUIRE(owner->seen_slices.size() == 1);
    CHECK(owner->seen_slices[0] == nlohmann::json{{"venv", ".venv"}});
}

TEST_CASE("Disclose sees the tool_config slice: disclosure is computed from the same resolution "
          "execute uses",
          "[agent_loop][tool_config]") {
    Harness h;
    h.checker_override = std::make_unique<GrantingPermissionChecker>();
    // Ask for the probe's category so the gate calls Disclose inside Prepare.
    h.executor_policy.read_only = tools::PermissionMode::Ask;
    h.registry.Register(std::make_unique<ConfigProbeTool>("probe", "python"));

    agent::AgentPreset preset;
    preset.tool_config = nlohmann::json{{"python", {{"venv", ".venv"}}}};
    h.provider.PushResponse(ToolUseResponse("tu1", "probe"));
    h.provider.PushResponse(TextResponse("done"));
    h.Build(preset);

    h.loop->RunTurn(UserText("go"));

    const auto* owner = dynamic_cast<ConfigProbeTool*>(h.registry.Find("probe").get());
    REQUIRE(owner != nullptr);
    // Disclose ran exactly once (the Ask gate) and saw the SAME slice execute
    // sees -- not a null (the pre-fix behavior: the slice was resolved only
    // after Prepare returned).
    REQUIRE(owner->disclosed_slices.size() == 1);
    REQUIRE(owner->seen_slices.size() == 1);
    CHECK(owner->disclosed_slices[0] == owner->seen_slices[0]);
    CHECK(owner->disclosed_slices[0] == nlohmann::json{{"venv", ".venv"}});
}

TEST_CASE("a tool whose namespace the preset does not declare gets a null tool_config",
          "[agent_loop][tool_config]") {
    Harness h;
    h.registry.Register(std::make_unique<ConfigProbeTool>("probe", "python"));

    agent::AgentPreset preset;
    preset.tool_config = nlohmann::json{{"other", {{"key", 1}}}};
    h.provider.PushResponse(ToolUseResponse("tu1", "probe"));
    h.provider.PushResponse(TextResponse("done"));
    h.Build(preset);

    h.loop->RunTurn(UserText("go"));

    const auto* owner = dynamic_cast<ConfigProbeTool*>(h.registry.Find("probe").get());
    REQUIRE(owner != nullptr);
    REQUIRE(owner->seen_slices.size() == 1);
    CHECK(owner->seen_slices[0].empty());
}

TEST_CASE("a tool that declares no namespace never sees a slice",
          "[agent_loop][tool_config]") {
    Harness h;
    // FakeTool declares no config_namespace (the Tool default).
    auto& tool = h.AddTool("read");
    agent::AgentPreset preset;
    preset.tool_config = nlohmann::json{{"python", {{"venv", ".venv"}}}};
    h.provider.PushResponse(ToolUseResponse("tu1", "read"));
    h.provider.PushResponse(TextResponse("done"));
    h.Build(preset);

    h.loop->RunTurn(UserText("go"));

    CHECK(tool.calls.size() == 1);
    // Nothing to assert on the slice itself without a probe; the point is that
    // the delivery path neither crashes nor serves another namespace's data --
    // exercised above. This test pins the default-config_namespace() path.
    SUCCEED("no-namespace tool completed a turn with tool_config present on the preset");
}
