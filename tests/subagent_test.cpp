// Subagent spawning, end to end but in-process: a real SessionRegistry and
// SessionDatabase, real Sessions with worker threads, scripted providers -- no
// socket. The socket tier comes later (headless attach); this tier proves the
// spawn paths themselves:
//
//   - blocking spawn returns the child's final text as the parent's result;
//   - non-blocking completion is injected at a turn boundary;
//   - the concurrency limit is an error the model adapts to, not a queue;
//   - a child can never spawn, never outrun its parent's permissions, and
//     inherits its grants;
//   - termination and cancellation unblock a waiting parent instead of hanging.

#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "deadline.hpp"
#include "../plugins/server/commands-core/core_commands.hpp"
#include "fakes.hpp"
#include "minimal/commands/command_registry.hpp"
#include "minimal/agent/session.hpp"
#include "minimal/agent/session_registry.hpp"
#include "minimal/agent/spawn_subagent_tool.hpp"
#include "minimal/agent/subagent_manager.hpp"
#include "minimal/common/logger.hpp"
#include "temp_dir.hpp"

using namespace minimal;
using namespace std::chrono_literals;

namespace {

constexpr const char* kSubagentPrefix = "SUBAGENT MODE. Report back one final summary.";

// ScriptedProvider (tests/support/fakes.hpp) consumes a SHARED script per
// model key: a blocking child reusing its parent's preset pulls from the
// parent's queue in real time, which is what makes interleaved parent/child
// scripting deterministic.
using Script = testing::ScriptedProviderScript;
using ScriptedProvider = testing::ScriptedProvider;

// --- Fixture --------------------------------------------------------------------

struct SubagentFixture {
    explicit SubagentFixture(std::int64_t max_concurrent = 4,
                             nlohmann::json permissions =
                                 nlohmann::json{{"per_tool_overrides",
                                                 {{"spawn_subagent", "allow"}}}})
        : skills(config::SkillRegistry::Load(dir.path())) {
        std::filesystem::create_directories(dir.path() / "agents");
        std::filesystem::create_directories(workdir);

        Write("providers.json",
              nlohmann::json{{"providers",
                              {{"main",
                                {{"backend", "fake"},
                                 {"url", "http://localhost/v1"},
                                 {"models", {{"parent-script", nlohmann::json::object()}}}}},
                               {"childmain",
                                {{"backend", "fake"},
                                 {"url", "http://localhost/v1"},
                                 {"models", {{"child-script", nlohmann::json::object()}}}}}}}}
                  .dump());
        Write("settings.json", nlohmann::json{{"default_agent_preset", "main"},
                                              {"max_concurrent_subagents", max_concurrent}}
                                   .dump());
        Write("permissions.json", permissions.dump());
        // model doubles as the script key: the factory routes each session's
        // provider at its script by it.
        Write("agents/main.md",
              "---\nname: main\nmodel: main/parent-script\nallowed_tools: "
              "[\"*\"]\n---\nYou are the main agent.\n");
        Write("agents/child.md",
              "---\nname: child\nmodel: childmain/child-script\nallowed_tools: "
              "[\"*\"]\n---\nYou are the child agent.\n");
        // execute: allow at the preset level -- only the PARENT's policy can
        // still hold it back (the ceiling tests).
        Write("agents/child-allow.md",
              "---\nname: child-allow\nmodel: childmain/child-script\nallowed_tools: "
              "[\"*\"]\npermission_mode:\n  execute: allow\n---\nYou are the child agent.\n");
        Write("agents/subagent.md", std::string(kSubagentPrefix) + "\n");

        // Both scripts exist before any provider is constructed: the map is
        // never mutated after this, so concurrent factory calls only read it.
        scripts["parent-script"] = std::make_shared<Script>();
        scripts["child-script"] = std::make_shared<Script>();

        providers.Register("fake", [scripts = this->scripts](const llm::ProviderConfig& cfg)
                                       -> std::unique_ptr<llm::LlmProvider> {
            const auto it = scripts.find(cfg.model_name);
            if (it == scripts.end()) return nullptr;
            return std::make_unique<ScriptedProvider>(it->second);
        });
    }

    void Build() {
        presets = std::make_unique<agent::AgentPresetRegistry>(
            dir.path(), skills, config::LoadSettingsConfig(dir.path() / "settings.json"));
        observers = std::make_unique<agent::SessionObserverRegistry>(common::DefaultLogger());
        db = std::make_unique<session::SessionDatabase>(dir.path() / "sessions.db");
        // A long guard: unattached children are normal here (no client in this
        // tier), and a test parked on a permission prompt must not be reaped
        // mid-assertion. Attach-guard behaviour has its own tests.
        sessions = std::make_unique<agent::SessionRegistry>(*presets, tools, providers,
                                                            *observers, *db, agent::LoopConfig{},
                                                            common::DefaultLogger(), 60s);
        manager = std::make_unique<agent::SubagentManager>(*sessions, *db, *presets,
                                                           /*max_concurrent_subagents=*/4,
                                                           common::DefaultLogger());
        agent::SubagentManager* mgr = manager.get();
        tools.Register(std::make_unique<agent::SpawnSubagentTool>(
            [mgr](const agent::SubagentRequest& request, const tools::ToolContext& ctx) {
                return mgr->Spawn(request, ctx);
            }));
    }

    void BuildWithLimit(std::int64_t max_concurrent) {
        presets = std::make_unique<agent::AgentPresetRegistry>(
            dir.path(), skills, config::LoadSettingsConfig(dir.path() / "settings.json"));
        observers = std::make_unique<agent::SessionObserverRegistry>(common::DefaultLogger());
        db = std::make_unique<session::SessionDatabase>(dir.path() / "sessions.db");
        sessions = std::make_unique<agent::SessionRegistry>(*presets, tools, providers,
                                                            *observers, *db, agent::LoopConfig{},
                                                            common::DefaultLogger(), 60s);
        manager = std::make_unique<agent::SubagentManager>(*sessions, *db, *presets,
                                                           max_concurrent,
                                                           common::DefaultLogger());
        agent::SubagentManager* mgr = manager.get();
        tools.Register(std::make_unique<agent::SpawnSubagentTool>(
            [mgr](const agent::SubagentRequest& request, const tools::ToolContext& ctx) {
                return mgr->Spawn(request, ctx);
            }));
    }

    std::shared_ptr<agent::Session> CreateParent(
        std::optional<tools::PermissionRunMode> run_mode = std::nullopt) {
        const auto row = db->CreateSession("", std::nullopt, workdir.string(), "main");
        agent::SessionParams params;
        params.session_id = row.session_id;
        params.agent_preset = "main";
        params.project_root = workdir;
        params.run_mode_override = run_mode;
        auto session = sessions->Create(params);
        session->TryAttach();  // a client is present in every designed flow
        return session;
    }

    std::vector<v1::ServerEvent> DrainQueue(agent::Session& session) {
        std::vector<v1::ServerEvent> out;
        while (auto event = session.queue().TryPop()) out.push_back(std::move(*event));
        return out;
    }

    // Drains until SubagentStartedEvent shows up, returning the child id. The
    // event is pushed before the child produces anything, so this is also the
    // attach-obligation starting gun.
    std::optional<std::string> WaitForSubagentStarted(agent::Session& parent,
                                                      bool* blocking_out = nullptr) {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline) {
            for (const auto& event : DrainQueue(parent)) {
                if (event.has_subagent_started()) {
                    if (blocking_out != nullptr) {
                        *blocking_out = event.subagent_started().blocking();
                    }
                    return event.subagent_started().child_session_id();
                }
            }
            std::this_thread::sleep_for(5ms);
        }
        return std::nullopt;
    }

    // Answers the next permission request on a session's queue, optionally
    // taking grants. Parks the caller until the prompt is visible.
    std::optional<std::string> WaitForPermissionRequest(agent::Session& session) {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline) {
            for (const auto& event : DrainQueue(session)) {
                if (event.has_permission_request()) return event.permission_request().request_id();
            }
            std::this_thread::sleep_for(5ms);
        }
        return std::nullopt;
    }

    void AnswerPermission(agent::Session& session, const std::string& request_id,
                          std::vector<std::string> granted_keys = {}) {
        v1::PermissionResponse answer;
        answer.set_request_id(request_id);
        answer.set_decision(v1::PERMISSION_DECISION_ALLOW);
        for (auto& key : granted_keys) answer.add_granted_keys(std::move(key));
        REQUIRE(session.pending().Resolve(request_id,
                                          rpc::PermissionAnswer{answer.SerializeAsString()}));
    }

    void Write(const std::filesystem::path& relative, const std::string& contents) {
        std::ofstream(dir.path() / relative) << contents;
    }

    testing::TempDir dir;
    std::filesystem::path workdir = dir.path() / "work";
    config::SkillRegistry skills;
    tools::ToolRegistry tools;
    llm::ProviderFactoryRegistry providers;
    std::map<std::string, std::shared_ptr<Script>> scripts;
    std::unique_ptr<agent::AgentPresetRegistry> presets;
    std::unique_ptr<agent::SessionObserverRegistry> observers;
    std::unique_ptr<session::SessionDatabase> db;
    std::unique_ptr<agent::SessionRegistry> sessions;
    std::unique_ptr<agent::SubagentManager> manager;
};

const llm::Message* FirstUserMessage(std::span<const llm::Message> messages) {
    for (const auto& message : messages) {
        if (message.role == llm::Role::User) return &message;
    }
    return nullptr;
}

std::string TextOf(const llm::ToolResultBlock& block) {
    std::string out;
    for (const auto& content : block.content) {
        if (const auto* text = std::get_if<llm::TextBlock>(&content)) out += text->text;
    }
    return out;
}

}  // namespace

// --- the spawn tool in isolation ------------------------------------------------

TEST_CASE("spawn_subagent validates its input before anything runs", "[subagent][tool]") {
    bool ran = false;
    agent::SpawnSubagentTool tool([&](const agent::SubagentRequest& request,
                                      const tools::ToolContext&) {
        ran = true;
        agent::SubagentOutcome outcome;
        outcome.kind = agent::SubagentOutcome::Kind::kStarted;
        outcome.child_session_id = "child-1";
        (void)request;
        return outcome;
    });
    tools::ToolContext ctx{common::DefaultLogger()};

    CHECK(tool.execute(nlohmann::json::object(), ctx).is_error);
    CHECK(tool.execute({{"task", 42}}, ctx).is_error);
    CHECK(tool.execute({{"task", "   \n"}}, ctx).is_error);
    CHECK(tool.execute({{"task", "x"}, {"blocking", "yes"}}, ctx).is_error);
    CHECK(tool.execute({{"task", "x"}, {"max_turns", -1}}, ctx).is_error);
    CHECK(tool.execute({{"task", "x"}, {"max_turns", 1.5}}, ctx).is_error);
    CHECK(tool.execute({{"task", "x"}, {"agent_preset", 3}}, ctx).is_error);
    CHECK_FALSE(ran);

    CHECK(tool.spec().category == tools::ToolCategory::Execute);
}

TEST_CASE("spawn_subagent maps runner outcomes onto tool results", "[subagent][tool]") {
    agent::SubagentRequest seen;
    agent::SubagentOutcome scripted;
    agent::SpawnSubagentTool tool([&](const agent::SubagentRequest& request,
                                      const tools::ToolContext&) {
        seen = request;
        return scripted;
    });
    tools::ToolContext ctx{common::DefaultLogger()};

    scripted.kind = agent::SubagentOutcome::Kind::kCompleted;
    scripted.final_text = "the child's report";
    auto result = tool.execute({{"task", "do it"}, {"blocking", true}, {"max_turns", 7}}, ctx);
    CHECK_FALSE(result.is_error);
    CHECK(result.content_text == "the child's report");
    CHECK(seen.task == "do it");
    CHECK(seen.blocking);
    CHECK(seen.max_turns == 7);

    // A child that stopped with nothing to say must not read like success.
    scripted.kind = agent::SubagentOutcome::Kind::kCompleted;
    scripted.final_text.clear();
    CHECK(tool.execute({{"task", "do it"}}, ctx).is_error);

    scripted.kind = agent::SubagentOutcome::Kind::kStarted;
    scripted.child_session_id = "child-9";
    result = tool.execute({{"task", "do it"}, {"blocking", false}}, ctx);
    CHECK_FALSE(result.is_error);
    CHECK(result.content_text.find("child-9") != std::string::npos);
    CHECK_FALSE(seen.blocking);

    scripted = agent::SubagentOutcome::Error("subagent limit reached (4 concurrent)");
    result = tool.execute({{"task", "do it"}}, ctx);
    CHECK(result.is_error);
    CHECK(result.content_text.find("limit") != std::string::npos);
}

TEST_CASE("spawn_subagent disclosure carries the task's first line", "[subagent][tool]") {
    agent::SpawnSubagentTool tool([](const agent::SubagentRequest&, const tools::ToolContext&) {
        return agent::SubagentOutcome::Error("unused");
    });
    tools::ToolContext ctx{common::DefaultLogger()};
    const auto disclosure = tool.Disclose({{"task", "refactor the parser\nand the tests"}}, ctx);
    CHECK(disclosure.summary.find("refactor the parser") != std::string::npos);
    CHECK(disclosure.summary.find('\n') == std::string::npos);
}

// --- blocking spawn ---------------------------------------------------------------

TEST_CASE("a blocking subagent returns its final text as the parent's tool result",
          "[subagent][blocking]") {
    SubagentFixture fixture;
    fixture.Build();
    auto parent = fixture.CreateParent();
    auto* parent_script = fixture.scripts["parent-script"].get();

    // The child uses the parent's own preset, so both consume ONE shared queue
    // in real time: parent call 1 (the spawn), the child's only call, parent
    // call 2. The blocking wait is what forces that order.
    parent_script->Push(testing::ToolUseResponse(
        "call-spawn", "spawn_subagent", {{"task", "do the thing"}}));
    parent_script->Push(testing::TextResponse("child result"));
    parent_script->Push(testing::TextResponse("parent done"));

    parent->SubmitTurn("go");
    REQUIRE(parent->WaitForIdleForTesting(10s));

    // Transcript: user, assistant(spawn), user(tool_result = child's text),
    // assistant. The child's whole life is the middle two rows of its own.
    const auto& stored = fixture.db->GetMessagesPage(parent->info().session_id, std::nullopt, 50);
    REQUIRE(stored.messages.size() == 4);
    // Page is descending by seq; oldest last.
    const auto& result_message = stored.messages[1].message;
    REQUIRE(result_message.role == llm::Role::User);
    REQUIRE(result_message.content.size() == 1);
    const auto& block = std::get<llm::ToolResultBlock>(result_message.content.front());
    CHECK(block.tool_use_id == "call-spawn");
    CHECK_FALSE(block.is_error);
    CHECK(TextOf(block) == "child result");

    // The child row exists, points at its parent, and was named from the task.
    bool blocking_flag = false;
    const auto child_id = fixture.WaitForSubagentStarted(*parent, &blocking_flag);
    REQUIRE(child_id.has_value());
    CHECK(blocking_flag);
    const auto child_row = fixture.db->GetSession(*child_id);
    REQUIRE(child_row.has_value());
    CHECK(child_row->parent_session_id == parent->info().session_id);
    CHECK(child_row->name == "do the thing");
    CHECK(fixture.sessions->ChildCount(parent->info().session_id) == 1);

    // The child's one provider call: prefixed system prompt, the task as its
    // seed, and NO spawn_subagent among its tools even though allowed_tools is
    // "*" and the tool is registered. Remove the is_subagent exclusion in
    // BuildChatRequest and this fails.
    const auto& requests = parent_script->requests;
    REQUIRE(requests.size() == 3);
    const auto& child_request = requests[1];
    REQUIRE(child_request.system.has_value());
    CHECK(child_request.system->rfind(kSubagentPrefix, 0) == 0);
    REQUIRE(FirstUserMessage(child_request.messages) != nullptr);
    CHECK(std::get<llm::TextBlock>(FirstUserMessage(child_request.messages)->content.front())
              .text == "do the thing");
    for (const auto& def : child_request.tools) {
        CHECK(def.name != "spawn_subagent");
    }
    // ...while the parent DID have it.
    bool parent_had_spawn = false;
    for (const auto& def : requests[0].tools) {
        if (def.name == "spawn_subagent") parent_had_spawn = true;
    }
    CHECK(parent_had_spawn);

    // And the parent's next request saw the result in its history.
    const auto& parent_second = requests[2];
    bool saw_result = false;
    for (const auto& message : parent_second.messages) {
        for (const auto& content : message.content) {
            if (const auto* result = std::get_if<llm::ToolResultBlock>(&content)) {
                if (result->tool_use_id == "call-spawn") saw_result = true;
            }
        }
    }
    CHECK(saw_result);
}

// --- non-blocking spawn -------------------------------------------------------------

TEST_CASE("a non-blocking subagent's completion is injected at a turn boundary",
          "[subagent][nonblocking]") {
    SubagentFixture fixture;
    fixture.Build();
    auto parent = fixture.CreateParent();
    auto* parent_script = fixture.scripts["parent-script"].get();
    auto* child_script = fixture.scripts["child-script"].get();

    parent_script->Push(testing::ToolUseResponse(
        "call-spawn", "spawn_subagent",
        {{"task", "work in the background"}, {"blocking", false}, {"agent_preset", "child"}}));
    parent_script->Push(testing::TextResponse("parent continues"));
    child_script->Push(testing::TextResponse("background result"));

    parent->SubmitTurn("go");
    REQUIRE(parent->WaitForIdleForTesting(10s));

    bool blocking_flag = true;
    const auto child_id = fixture.WaitForSubagentStarted(*parent, &blocking_flag);
    REQUIRE(child_id.has_value());
    CHECK_FALSE(blocking_flag);

    // The acknowledgment is NOT the child's result. Found by content rather than
    // position: a fast child's completion injection can land in this same turn,
    // shifting rows.
    {
        const auto page = fixture.db->GetMessagesPage(parent->info().session_id, std::nullopt, 50);
        bool saw_ack = false;
        for (const auto& stored : page.messages) {
            if (stored.message.role != llm::Role::User) continue;
            for (const auto& content : stored.message.content) {
                const auto* result = std::get_if<llm::ToolResultBlock>(&content);
                if (result == nullptr || result->tool_use_id != "call-spawn") continue;
                if (TextOf(*result).find("started") != std::string::npos) {
                    saw_ack = true;
                    CHECK_FALSE(result->is_error);
                }
            }
        }
        CHECK(saw_ack);
    }

    // Let the child finish, then run a second parent turn: the completion must
    // reach the parent's history whole -- never between a tool_use and its
    // tool_result, which would be a hard 400 on both wire formats.
    auto child = fixture.sessions->Find(*child_id);
    REQUIRE(child != nullptr);
    REQUIRE(child->WaitForIdleForTesting(10s));

    parent_script->Push(testing::TextResponse("turn two"));
    parent->SubmitTurn("next");
    REQUIRE(parent->WaitForIdleForTesting(10s));

    std::vector<llm::Message> history;
    {
        auto page = fixture.db->GetMessagesPage(parent->info().session_id, std::nullopt, 100);
        for (auto it = page.messages.rbegin(); it != page.messages.rend(); ++it) {
            history.push_back(it->message);
        }
    }

    bool saw_injection = false;
    for (std::size_t i = 0; i < history.size(); ++i) {
        const auto& message = history[i];
        if (message.role != llm::Role::User) continue;
        for (const auto& content : message.content) {
            const auto* result = std::get_if<llm::ToolResultBlock>(&content);
            if (result == nullptr || result->tool_use_id != "call-spawn") continue;
            // The ack already used this tool_use_id; the injection is the one
            // carrying the child's actual text.
            if (TextOf(*result) == "background result") {
                saw_injection = true;
                CHECK_FALSE(result->is_error);
            }
        }
        // Boundary invariant, checked at every assistant tool call: the message
        // after an assistant message's tool_use blocks is the user message
        // holding their results -- nothing injected in between.
        if (message.role == llm::Role::Assistant && i + 1 < history.size()) {
            for (const auto& content : message.content) {
                const auto* use = std::get_if<llm::ToolUseBlock>(&content);
                if (use == nullptr) continue;
                const auto& next = history[i + 1];
                REQUIRE(next.role == llm::Role::User);
                bool paired = false;
                for (const auto& next_content : next.content) {
                    if (const auto* r = std::get_if<llm::ToolResultBlock>(&next_content)) {
                        if (r->tool_use_id == use->id) paired = true;
                    }
                }
                CHECK(paired);
            }
        }
    }
    CHECK(saw_injection);
}

// --- limits ---------------------------------------------------------------------

TEST_CASE("an over-limit spawn is an error result, not a queue", "[subagent][limit]") {
    SubagentFixture fixture;
    fixture.BuildWithLimit(1);
    auto parent = fixture.CreateParent();
    auto* parent_script = fixture.scripts["parent-script"].get();
    auto* child_script = fixture.scripts["child-script"].get();

    llm::ChatResponse two_spawns;
    two_spawns.content.push_back(llm::ToolUseBlock{
        "sp1", "spawn_subagent",
        {{"task", "first"}, {"blocking", false}, {"agent_preset", "child"}}});
    two_spawns.content.push_back(llm::ToolUseBlock{
        "sp2", "spawn_subagent",
        {{"task", "second"}, {"blocking", false}, {"agent_preset", "child"}}});
    two_spawns.stop_reason = llm::StopReason::ToolUse;
    two_spawns.usage.input_tokens = 10;
    two_spawns.usage.output_tokens = 5;

    parent_script->Push(std::move(two_spawns));
    parent_script->Push(testing::TextResponse("adapted"));
    child_script->Push(testing::TextResponse("first result"));

    parent->SubmitTurn("go");
    REQUIRE(parent->WaitForIdleForTesting(10s));

    // One child live, one spawn refused with a reason the model can act on.
    CHECK(fixture.sessions->ChildCount(parent->info().session_id) == 1);

    // The two results, found by content rather than position: whether the live
    // child's completion injection landed during this turn shifts rows.
    const auto page = fixture.db->GetMessagesPage(parent->info().session_id, std::nullopt, 50);
    bool saw_results = false;
    for (const auto& stored : page.messages) {
        if (stored.message.role != llm::Role::User || stored.message.content.size() != 2) {
            continue;
        }
        const auto* first = std::get_if<llm::ToolResultBlock>(&stored.message.content[0]);
        const auto* second = std::get_if<llm::ToolResultBlock>(&stored.message.content[1]);
        if (first == nullptr || second == nullptr) continue;
        if (first->tool_use_id != "sp1" || second->tool_use_id != "sp2") continue;
        saw_results = true;
        CHECK_FALSE(first->is_error);
        CHECK(second->is_error);
        CHECK(TextOf(*second).find("limit") != std::string::npos);
    }
    CHECK(saw_results);
}

// --- permission inheritance -------------------------------------------------------

TEST_CASE("a child of an AllowOnly parent auto-denies instead of prompting",
          "[subagent][permissions]") {
    // The parent runs AllowOnly (headless posture). The child's Ask-gated tool
    // must inherit that: re-prompting on a stream nobody answers would hang the
    // child, and letting it prompt at all would make the child MORE permissive
    // than its parent.
    SubagentFixture fixture(4, nlohmann::json{
                                   {"fallback", {{"execute", "ask"}}},
                                   {"per_tool_overrides", {{"spawn_subagent", "allow"}}}});
    fixture.Build();
    auto parent = fixture.CreateParent(tools::PermissionRunMode::AllowOnly);

    auto bash = std::make_unique<testing::FakeTool>("bash", tools::ToolCategory::Execute, "ran");
    auto* bash_ptr = bash.get();
    fixture.tools.Register(std::move(bash));

    auto* script = fixture.scripts["parent-script"].get();
    script->Push(testing::ToolUseResponse("call-spawn", "spawn_subagent", {{"task", "run it"}}));
    script->Push(testing::ToolUseResponse("call-bash", "bash"));
    script->Push(testing::TextResponse("child gave up"));
    script->Push(testing::TextResponse("parent done"));

    parent->SubmitTurn("go");
    REQUIRE(parent->WaitForIdleForTesting(10s));  // nothing blocked on a prompt

    CHECK(bash_ptr->calls.empty());  // Ask clamped to AllowOnly -> auto-denied
    const auto page = fixture.db->GetMessagesPage(parent->info().session_id, std::nullopt, 50);
    const auto& result_row = page.messages[1].message;
    const auto& block = std::get<llm::ToolResultBlock>(result_row.content.front());
    CHECK(TextOf(block) == "child gave up");
}

TEST_CASE("the parent's policy is the child's ceiling even against the child preset",
          "[subagent][permissions]") {
    // The child preset says execute: allow; the parent's policy says execute:
    // ask. The ceiling must win -- and the proof is the prompt itself, because
    // removing the ceiling fold lets the tool run unprompted.
    SubagentFixture fixture(4, nlohmann::json{
                                   {"fallback", {{"execute", "ask"}}},
                                   {"per_tool_overrides", {{"spawn_subagent", "allow"}}}});
    fixture.Build();
    auto parent = fixture.CreateParent();

    auto bash = std::make_unique<testing::FakeTool>("bash", tools::ToolCategory::Execute, "ran");
    auto* bash_ptr = bash.get();
    fixture.tools.Register(std::move(bash));

    auto* parent_script = fixture.scripts["parent-script"].get();
    auto* child_script = fixture.scripts["child-script"].get();
    parent_script->Push(testing::ToolUseResponse(
        "call-spawn", "spawn_subagent", {{"task", "run it"}, {"agent_preset", "child-allow"}}));
    parent_script->Push(testing::TextResponse("parent done"));
    child_script->Push(testing::ToolUseResponse("call-bash", "bash"));
    child_script->Push(testing::TextResponse("child result"));

    parent->SubmitTurn("go");
    const auto child_id = fixture.WaitForSubagentStarted(*parent);
    REQUIRE(child_id.has_value());
    auto child = fixture.sessions->Find(*child_id);
    REQUIRE(child != nullptr);

    // The child parks on the prompt the ceiling forced.
    const auto request_id = fixture.WaitForPermissionRequest(*child);
    REQUIRE(request_id.has_value());
    CHECK(bash_ptr->calls.empty());

    fixture.AnswerPermission(*child, *request_id);
    REQUIRE(parent->WaitForIdleForTesting(10s));
    CHECK(bash_ptr->calls.size() == 1);

    const auto page = fixture.db->GetMessagesPage(parent->info().session_id, std::nullopt, 50);
    const auto& block =
        std::get<llm::ToolResultBlock>(page.messages[1].message.content.front());
    CHECK(TextOf(block) == "child result");
}

TEST_CASE("a child inherits the grants its parent already held", "[subagent][grants]") {
    // Copy-at-spawn: the parent's operator approved a binary once; the child
    // must not ask again. Without the seed the child would park on the prompt
    // and this test would time out instead of passing.
    SubagentFixture fixture(4, nlohmann::json{
                                   {"fallback", {{"execute", "ask"}}},
                                   {"per_tool_overrides", {{"spawn_subagent", "allow"}}}});
    fixture.Build();
    auto parent = fixture.CreateParent();

    auto bash = std::make_unique<testing::FakeTool>("bash", tools::ToolCategory::Execute, "ran");
    tools::PermissionDisclosure disclosure;
    disclosure.summary = "a command";
    disclosure.grants.push_back(tools::GrantOffer{"bash:/usr/bin/tool", "/usr/bin/tool *"});
    bash->set_disclosure(std::move(disclosure));
    auto* bash_ptr = bash.get();
    fixture.tools.Register(std::move(bash));

    // One assistant turn asking for two things in sequence: the gated tool
    // (answered with a grant), then a blocking spawn of a child that runs the
    // same tool.
    llm::ChatResponse bash_then_spawn;
    bash_then_spawn.content.push_back(llm::ToolUseBlock{"b1", "bash", {{"command", "tool"}}});
    bash_then_spawn.content.push_back(
        llm::ToolUseBlock{"sp1", "spawn_subagent", {{"task", "same again"}}});
    bash_then_spawn.stop_reason = llm::StopReason::ToolUse;
    bash_then_spawn.usage.input_tokens = 10;
    bash_then_spawn.usage.output_tokens = 5;

    auto* script = fixture.scripts["parent-script"].get();
    script->Push(std::move(bash_then_spawn));
    script->Push(testing::ToolUseResponse("b2", "bash", {{"command", "tool"}}));
    script->Push(testing::TextResponse("child done"));
    script->Push(testing::TextResponse("parent done"));

    parent->SubmitTurn("go");

    // The parent's own prompt: answered, and the grant taken.
    const auto request_id = fixture.WaitForPermissionRequest(*parent);
    REQUIRE(request_id.has_value());
    fixture.AnswerPermission(*parent, *request_id, {"bash:/usr/bin/tool"});

    REQUIRE(parent->WaitForIdleForTesting(10s));
    CHECK(bash_ptr->calls.size() == 2);  // parent's call AND child's, silently

    // The child never emitted a permission request.
    const auto child_id = [&] {
        // SubagentStartedEvent was drained already by WaitForPermissionRequest?
        // No -- that helper only scans for prompts, but it DRAINS everything.
        // Re-scan is impossible, so find the child through the registry.
        std::string found;
        // ChildCount is 1; the id comes from the database.
        const auto sessions_list = fixture.db->ListSessions({.include_subagents = true});
        for (const auto& row : sessions_list) {
            if (row.parent_session_id == parent->info().session_id) found = row.session_id;
        }
        return found;
    }();
    REQUIRE_FALSE(child_id.empty());
    auto child = fixture.sessions->Find(child_id);
    REQUIRE(child != nullptr);
    for (const auto& event : fixture.DrainQueue(*child)) {
        CHECK_FALSE(event.has_permission_request());
    }
}

// --- termination and cancellation ----------------------------------------------------

TEST_CASE("terminating a blocking subagent unblocks its parent with an error result",
          "[subagent][lifetime]") {
    SubagentFixture fixture(4, nlohmann::json{
                                   {"fallback", {{"execute", "ask"}}},
                                   {"per_tool_overrides", {{"spawn_subagent", "allow"}}}});
    fixture.Build();
    auto parent = fixture.CreateParent();

    auto* parent_script = fixture.scripts["parent-script"].get();
    auto* child_script = fixture.scripts["child-script"].get();
    parent_script->Push(testing::ToolUseResponse(
        "call-spawn", "spawn_subagent", {{"task", "long job"}, {"agent_preset", "child"}}));
    parent_script->Push(testing::TextResponse("after"));
    // The child parks on an unanswered permission prompt: a deterministic
    // "still running" that needs no sleeps.
    child_script->Push(testing::ToolUseResponse("call-bash", "bash"));
    child_script->Push(testing::TextResponse("never"));

    fixture.tools.Register(
        std::make_unique<testing::FakeTool>("bash", tools::ToolCategory::Execute, "ran"));

    parent->SubmitTurn("go");
    const auto child_id = fixture.WaitForSubagentStarted(*parent);
    REQUIRE(child_id.has_value());
    REQUIRE(fixture.WaitForPermissionRequest(*fixture.sessions->Find(*child_id)).has_value());

    // Closing the child's tab terminates it; the parent must unblock with an
    // error rather than hang on its synchronous wait.
    fixture.sessions->Terminate(*child_id);
    REQUIRE(parent->WaitForIdleForTesting(10s));

    const auto page = fixture.db->GetMessagesPage(parent->info().session_id, std::nullopt, 50);
    // Descending: [assistant "after", user(result), assistant(spawn), user "go"].
    REQUIRE(page.messages.size() == 4);
    const auto& block =
        std::get<llm::ToolResultBlock>(page.messages[1].message.content.front());
    CHECK(block.is_error);
    CHECK(TextOf(block).find("terminated") != std::string::npos);
    CHECK(fixture.sessions->ChildCount(parent->info().session_id) == 0);
}

TEST_CASE("cancelling a parent turn terminates its blocking child", "[subagent][lifetime]") {
    SubagentFixture fixture(4, nlohmann::json{
                                   {"fallback", {{"execute", "ask"}}},
                                   {"per_tool_overrides", {{"spawn_subagent", "allow"}}}});
    fixture.Build();
    auto parent = fixture.CreateParent();

    auto* parent_script = fixture.scripts["parent-script"].get();
    auto* child_script = fixture.scripts["child-script"].get();
    parent_script->Push(testing::ToolUseResponse(
        "call-spawn", "spawn_subagent", {{"task", "long job"}, {"agent_preset", "child"}}));
    child_script->Push(testing::ToolUseResponse("call-bash", "bash"));

    fixture.tools.Register(
        std::make_unique<testing::FakeTool>("bash", tools::ToolCategory::Execute, "ran"));

    parent->SubmitTurn("go");
    const auto child_id = fixture.WaitForSubagentStarted(*parent);
    REQUIRE(child_id.has_value());
    auto child = fixture.sessions->Find(*child_id);
    REQUIRE(child != nullptr);
    REQUIRE(fixture.WaitForPermissionRequest(*child).has_value());

    // CancelTurn while the parent is parked in the blocking wait. The wait
    // loop must observe the cancellation, terminate the child, and return --
    // in that order -- instead of waiting out a turn nobody wants any more.
    parent->Cancel();
    REQUIRE(parent->WaitForIdleForTesting(10s));
    CHECK(child->terminated());
    CHECK(fixture.sessions->ChildCount(parent->info().session_id) == 0);

    // The turn ended cancelled, and the session survives it.
    bool saw_cancelled_turn = false;
    for (const auto& event : fixture.DrainQueue(*parent)) {
        if (event.has_turn_completed() &&
            event.turn_completed().stop_reason() == v1::STOP_REASON_CANCELLED) {
            saw_cancelled_turn = true;
        }
    }
    CHECK(saw_cancelled_turn);
    CHECK_FALSE(parent->terminated());
}

// --- max_turns ---------------------------------------------------------------------

TEST_CASE("max_turns bounds the child's loop", "[subagent][max_turns]") {
    SubagentFixture fixture;
    fixture.Build();
    auto parent = fixture.CreateParent();

    fixture.tools.Register(
        std::make_unique<testing::FakeTool>("read", tools::ToolCategory::ReadOnly, "data"));

    auto* parent_script = fixture.scripts["parent-script"].get();
    auto* child_script = fixture.scripts["child-script"].get();
    parent_script->Push(testing::ToolUseResponse(
        "call-spawn", "spawn_subagent",
        {{"task", "loop forever"}, {"agent_preset", "child"}, {"max_turns", 2}}));
    parent_script->Push(testing::TextResponse("parent done"));
    child_script->Push(testing::ToolUseResponse("r1", "read"));
    child_script->Push(testing::ToolUseResponse("r2", "read"));
    child_script->Push(testing::ToolUseResponse("r3", "read"));  // never consumed

    parent->SubmitTurn("go");
    REQUIRE(parent->WaitForIdleForTesting(10s));

    // Two tool rounds, then the cap: no final text, so the parent sees an
    // error naming the reason rather than an empty success.
    const auto page = fixture.db->GetMessagesPage(parent->info().session_id, std::nullopt, 50);
    const auto& block =
        std::get<llm::ToolResultBlock>(page.messages[1].message.content.front());
    CHECK(block.is_error);
    CHECK(TextOf(block).find("max_turns") != std::string::npos);

    {
        std::lock_guard<std::mutex> lock(child_script->mu);
        CHECK(child_script->steps.size() == 1);  // r3 never asked for
    }
}

// --- preset errors -------------------------------------------------------------------

TEST_CASE("an unknown preset refuses the spawn without creating anything",
          "[subagent][preset]") {
    SubagentFixture fixture;
    fixture.Build();
    auto parent = fixture.CreateParent();

    auto* script = fixture.scripts["parent-script"].get();
    script->Push(testing::ToolUseResponse(
        "call-spawn", "spawn_subagent", {{"task", "doomed"}, {"agent_preset", "nonexistent"}}));
    script->Push(testing::TextResponse("parent done"));

    parent->SubmitTurn("go");
    REQUIRE(parent->WaitForIdleForTesting(10s));

    CHECK(fixture.sessions->ChildCount(parent->info().session_id) == 0);
    const auto page = fixture.db->GetMessagesPage(parent->info().session_id, std::nullopt, 50);
    const auto& block =
        std::get<llm::ToolResultBlock>(page.messages[1].message.content.front());
    CHECK(block.is_error);
    CHECK(TextOf(block).find("nonexistent") != std::string::npos);
}

TEST_CASE("a subagent gets no /-command surface", "[subagent][commands][invariant]") {
    // CROSS-FEATURE, and neither branch could have seen it: subagents and the
    // command system were built in parallel, and a child is constructed through
    // the same registry path that wires the command registry.
    //
    // Leaving it open is an ESCAPE. /agent re-resolves a preset from disk, and a
    // freshly resolved preset has is_subagent = false and its own full
    // allowed_tools -- so `/agent build` inside a child would hand back
    // spawn_subagent and reopen the recursion ARCHITECTURE.md calls a hard rule,
    // and would drop the intersected allowed lists the parent handed down.
    //
    // Commands are a user control channel and a child has no user: its client
    // attaches to observe. Closing the surface removes the class rather than
    // patching the one command that reaches it today.
    SubagentFixture fixture;
    commands::CommandRegistry registry;
    for (auto& command : commands_core::MakeCoreCommands()) registry.Register(std::move(command));
    registry.Register(commands_core::MakeHelpCommand(registry));
    fixture.Build();
    fixture.sessions->set_commands(&registry);

    auto parent = fixture.CreateParent();
    auto* parent_script = fixture.scripts["parent-script"].get();
    auto* child_script = fixture.scripts["child-script"].get();

    parent_script->Push(testing::ToolUseResponse(
        "call-spawn", "spawn_subagent",
        {{"task", "do a thing"}, {"blocking", false}, {"agent_preset", "child"}}));
    parent_script->Push(testing::TextResponse("parent continues"));
    child_script->Push(testing::TextResponse("child result"));

    parent->SubmitTurn("go");
    REQUIRE(parent->WaitForIdleForTesting(10s));
    const auto child_id = fixture.WaitForSubagentStarted(*parent);
    REQUIRE(child_id.has_value());
    auto child = fixture.sessions->Find(*child_id);
    REQUIRE(child != nullptr);
    REQUIRE(child->WaitForIdleForTesting(10s));

    const auto answer = [&fixture](agent::Session& session) {
        agent::QueuedCommand command;
        command.request_id = "probe";
        command.name = "help";
        session.RunCommandNow(command);
        for (const auto& event : fixture.DrainQueue(session)) {
            if (event.has_command_result() && event.command_result().request_id() == "probe") {
                return event.command_result().message();
            }
        }
        return std::string("<no answer>");
    };

    // The parent answers /help with its listing; the child answers that it has
    // no command subsystem at all. Both ANSWER -- a dropped invocation would
    // hang a client either way -- but only one of them can act.
    CHECK(answer(*parent).find("/agent") != std::string::npos);
    CHECK(answer(*child).find("not available") != std::string::npos);
}
