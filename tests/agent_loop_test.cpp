#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>

#include "fakes.hpp"
#include "minimal/agent/agent_loop.hpp"
#include "minimal/config/skill_registry.hpp"
#include "minimal/tools/skill_tool.hpp"
#include "temp_dir.hpp"

using namespace minimal;
using namespace minimal::testing;
using minimal::tools::PermissionRunMode;
using minimal::tools::ToolCategory;

namespace {

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
    InMemoryHistoryStore store;
    std::unique_ptr<tools::ToolExecutor> executor;
    std::unique_ptr<agent::AgentLoop> loop;
    std::vector<llm::StreamEvent> seen_events;
    bool cancel_flag = false;

    FakeTool& AddTool(std::string name, ToolCategory category = ToolCategory::ReadOnly) {
        auto owned = std::make_unique<FakeTool>(std::move(name), category);
        FakeTool* raw = owned.get();
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
               agent::Compactor compactor = {}) {
        if (preset.allowed_tools.empty()) preset.allowed_tools = {"*"};
        if (preset.provider.model_name.empty()) preset.provider.model_name = "test-model";
        preset.permission_mode = AllowAllPolicy();

        executor = std::make_unique<tools::ToolExecutor>(registry, checker, AllowAllPolicy(),
                                                         tools::McpPermissionPolicy{},
                                                         PermissionRunMode::Normal);
        agent::SessionContext session{common::DefaultLogger()};
        session.session_id = "s1";
        session.project_root = ".";

        loop = std::make_unique<agent::AgentLoop>(
            provider, *executor, registry, store, std::move(preset), config, std::move(session),
            [this] { return cancel_flag; },
            [this](const llm::StreamEvent& e) {
                seen_events.push_back(e);
                return !cancel_flag;
            },
            std::move(compactor));
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

TEST_CASE("retryable errors retry, non-retryable propagate", "[agent_loop][retry]") {
    SECTION("retryable then success") {
        Harness h;
        h.provider.PushError(llm::LlmError(llm::LlmErrorCategory::RateLimit, "429"));
        h.provider.PushResponse(TextResponse("recovered"));
        h.Build({}, agent::LoopConfig{.max_retries = 3});

        const auto result = h.loop->RunTurn(UserText("hi"));
        CHECK(result.final_text == "recovered");
        CHECK(h.provider.call_count() == 2);
    }

    SECTION("non-retryable aborts immediately") {
        Harness h;
        h.provider.PushError(llm::LlmError(llm::LlmErrorCategory::AuthError, "401"));
        h.provider.PushResponse(TextResponse("never reached"));
        h.Build({}, agent::LoopConfig{.max_retries = 3});

        CHECK_THROWS_AS(h.loop->RunTurn(UserText("hi")), llm::LlmError);
        CHECK(h.provider.call_count() == 1);
    }

    SECTION("retries are bounded") {
        Harness h;
        for (int i = 0; i < 5; ++i) {
            h.provider.PushError(llm::LlmError(llm::LlmErrorCategory::ServerError, "500"));
        }
        h.Build({}, agent::LoopConfig{.max_retries = 2});

        CHECK_THROWS_AS(h.loop->RunTurn(UserText("hi")), llm::LlmError);
        CHECK(h.provider.call_count() == 3);  // initial + 2 retries
    }
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
    auto compactor = [&compactions](const std::vector<llm::Message>&) {
        ++compactions;
        return UserText("[summary]");
    };
    // TextResponse reports 10 in + 5 out = occupancy 15.
    h.Build({}, agent::LoopConfig{.auto_compact = true, .compaction_trigger_tokens = 10},
            compactor);

    h.loop->RunTurn(UserText("a"));
    const std::size_t store_after_first = h.store.messages.size();
    h.loop->RunTurn(UserText("b"));

    CHECK(compactions >= 1);
    // Compaction rewrites only the in-memory working set. The durable record
    // keeps every message, so scrollback is unaffected however often it runs.
    CHECK(h.store.messages.size() > store_after_first);
}

TEST_CASE("auto_compact off means no compaction regardless of occupancy",
          "[agent_loop][compaction]") {
    Harness h;
    h.provider.PushResponse(TextResponse("one"));
    h.provider.PushResponse(TextResponse("two"));

    int compactions = 0;
    auto compactor = [&compactions](const std::vector<llm::Message>&) {
        ++compactions;
        return UserText("[summary]");
    };
    h.Build({}, agent::LoopConfig{.auto_compact = false, .compaction_trigger_tokens = 1},
            compactor);

    h.loop->RunTurn(UserText("a"));
    h.loop->RunTurn(UserText("b"));

    CHECK(compactions == 0);
}

TEST_CASE("a queued subagent result lands at a turn boundary", "[agent_loop][subagent]") {
    // Injecting out-of-band could place a user message between a tool_use and
    // its tool_result, which is a hard 400 on both wire formats.
    Harness h;
    h.AddTool("read");
    h.provider.PushResponse(ToolUseResponse("tu1", "read"));
    h.provider.PushResponse(TextResponse("done"));
    h.Build();

    h.loop->QueueInjectedMessage(UserText("[Subagent finished] result"));
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

TEST_CASE("the skill tool round-trips a lazy-loaded body through the loop", "[agent_loop][skills]") {
    Harness h;
    h.EnableSkills({{"deploy", "Always run the smoke tests first.\n"}});
    agent::AgentPreset preset;
    preset.allowed_skills = {"deploy"};
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
        preset.allowed_skills = {"deploy"};
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
    preset.allowed_skills = {"other"};
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
