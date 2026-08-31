// Subagents over a real socket: the tier where the wire itself is on trial. The
// in-process tier (subagent_test.cpp) proves the spawn machinery; this one
// proves the client protocol around it -- SubagentStartedEvent reaches a real
// client, the client ATTACHES to the child with the Attach payload (its
// obligation, and the only way the child escapes the initial-attach guard),
// the child's events flow on the child's own stream, and the parent gets the
// result. Closes Milestone A4's child-attach checkbox, which was unenforceable
// until subagents existed.

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include <catch2/catch_test_macros.hpp>

#include "fakes.hpp"
#include "minimal/agent/spawn_subagent_tool.hpp"
#include "minimal/agent/subagent_manager.hpp"
#include "minimal/client/session_client.hpp"
#include "minimal/rpc/server.hpp"
#include "minimal/rpc/session_service.hpp"

using namespace minimal;
using namespace std::chrono_literals;

namespace {

struct SubagentE2EFixture {
    SubagentE2EFixture() {
        // Unique per fixture: ctest runs cases in parallel, and a shared arena
        // means one case's remove_all() deleting another's socket mid-test.
        static std::atomic<int> counter{0};
        dir = std::filesystem::temp_directory_path() /
              ("minimal_subagent_e2e_arena-" + std::to_string(::getpid()) + "-" +
               std::to_string(counter.fetch_add(1)));
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir / "agents");
        std::filesystem::create_directories(dir / "work");

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
        Write("settings.json", nlohmann::json{{"default_agent_preset", "main"}}.dump());
        // spawn_subagent is Execute, Ask by default; AllowOnly would auto-deny
        // it, which is correct for bash but useless for a test about spawning.
        Write("permissions.json",
              nlohmann::json{{"per_tool_overrides", {{"spawn_subagent", "allow"}}}}.dump());
        // model doubles as the script key for the fake provider factory.
        Write("agents/main.md",
              "---\nname: main\nmodel: main/parent-script\nallowed_tools: "
              "[\"*\"]\n---\nYou are the main agent.\n");
        Write("agents/child.md",
              "---\nname: child\nmodel: childmain/child-script\nallowed_tools: "
              "[\"*\"]\n---\nYou are the child agent.\n");
        Write("agents/subagent.md", "You are running as a subagent.\n");

        scripts["parent-script"] = std::make_shared<testing::ScriptedProviderScript>();
        scripts["child-script"] = std::make_shared<testing::ScriptedProviderScript>();
    }

    void Write(const std::filesystem::path& relative, const std::string& contents) {
        std::ofstream out(dir / relative);
        out << contents;
    }

    void Start() {
        skills = config::SkillRegistry::Load(dir);
        presets = std::make_unique<agent::AgentPresetRegistry>(
            dir, skills, config::LoadSettingsConfig(dir / "settings.json"));
        providers.Register("fake", [scripts = this->scripts](const llm::ProviderConfig& cfg)
                                       -> std::unique_ptr<llm::LlmProvider> {
            const auto it = scripts.find(cfg.model_name);
            if (it == scripts.end()) return nullptr;
            return std::make_unique<testing::ScriptedProvider>(it->second);
        });
        observers = std::make_unique<agent::SessionObserverRegistry>(common::DefaultLogger());
        db = std::make_unique<session::SessionDatabase>(dir / "sessions.db");
        sessions = std::make_unique<agent::SessionRegistry>(*presets, tools, providers,
                                                            *observers, *db, agent::LoopConfig{},
                                                            common::DefaultLogger());
        manager = std::make_unique<agent::SubagentManager>(*sessions, *db, *presets, 4,
                                                           common::DefaultLogger());
        agent::SubagentManager* mgr = manager.get();
        tools.Register(std::make_unique<testing::FakeTool>("read", tools::ToolCategory::ReadOnly,
                                                           "file contents"));
        tools.Register(std::make_unique<agent::SpawnSubagentTool>(
            [mgr](const agent::SubagentRequest& request, const tools::ToolContext& ctx) {
                return mgr->Spawn(request, ctx);
            }));

        service = std::make_unique<rpc::SessionServiceImpl>(*sessions, *db,
                                                            config::SettingsConfig{},
                                                            common::DefaultLogger());
        socket = dir / "subagent.sock";
        server = std::make_unique<rpc::Server>(*service, socket);
        REQUIRE(server->listening());
        client = std::make_unique<client::SessionClient>(rpc::SocketTarget(socket));
        REQUIRE(client->WaitForConnected(5s));
    }

    ~SubagentE2EFixture() {
        for (auto& thread : attach_threads) {
            if (thread.joinable()) thread.join();
        }
        client.reset();
        if (server) server->Shutdown();
        server.reset();
        if (sessions) sessions->TerminateAll();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    std::filesystem::path dir, socket;
    std::map<std::string, std::shared_ptr<testing::ScriptedProviderScript>> scripts;
    config::SkillRegistry skills;
    tools::ToolRegistry tools;
    llm::ProviderFactoryRegistry providers;
    std::unique_ptr<agent::AgentPresetRegistry> presets;
    std::unique_ptr<agent::SessionObserverRegistry> observers;
    std::unique_ptr<session::SessionDatabase> db;
    std::unique_ptr<agent::SessionRegistry> sessions;
    std::unique_ptr<agent::SubagentManager> manager;
    std::unique_ptr<rpc::SessionServiceImpl> service;
    std::unique_ptr<rpc::Server> server;
    std::unique_ptr<client::SessionClient> client;
    std::vector<std::thread> attach_threads;
};

// What the attach thread saw on the child's stream.
struct ChildObservations {
    std::mutex mu;
    std::string session_id;
    std::string text;
    bool turn_completed = false;
};

std::string TextOf(const llm::ToolResultBlock& block) {
    std::string out;
    for (const auto& content : block.content) {
        if (const auto* text = std::get_if<llm::TextBlock>(&content)) out += text->text;
    }
    return out;
}

// The fake provider emits only the stream events it is handed; these are what
// reach the client as deltas.
std::vector<llm::StreamEvent> TextStream(const std::string& text, llm::StopReason stop) {
    return {llm::StreamEvent{llm::MessageStartEvent{"m-1", "test-model"}},
            llm::StreamEvent{llm::TextDeltaEvent{0, text}},
            llm::StreamEvent{llm::ContentBlockStopEvent{0}},
            llm::StreamEvent{llm::MessageDeltaEvent{stop}},
            llm::StreamEvent{llm::MessageStopEvent{}}};
}

}  // namespace

TEST_CASE("a client attaches to a blocking subagent over the wire", "[subagent][e2e]") {
    SubagentE2EFixture fixture;
    fixture.Start();
    auto* parent_script = fixture.scripts["parent-script"].get();
    auto* child_script = fixture.scripts["child-script"].get();

    parent_script->Push(testing::ToolUseResponse(
        "call-spawn", "spawn_subagent", {{"task", "find the answer"}, {"agent_preset", "child"}}));
    parent_script->Push(testing::TextResponse("all done"),
                        TextStream("all done", llm::StopReason::EndTurn));
    child_script->Push(testing::ToolUseResponse("r1", "read", {{"path", "x.txt"}}));
    child_script->Push(testing::TextResponse("child found it"),
                       TextStream("child found it", llm::StopReason::EndTurn));

    client::SessionClient::CreateOptions options;
    options.project_root = (fixture.dir / "work").string();
    options.agent_preset = "main";
    options.run_mode = v1::PERMISSION_RUN_MODE_ALLOW_ONLY;
    const auto created = fixture.client->CreateSession(options);
    REQUIRE(created.status.ok());

    auto observations = std::make_shared<ChildObservations>();
    std::string started_child_id;
    bool saw_started = false;
    std::string parent_text;

    const auto status = fixture.client->RunTurn(
        created.session_id, "go", [&](const v1::ServerEvent& event) {
            if (event.has_stream_event() && event.stream_event().has_text_delta()) {
                parent_text += event.stream_event().text_delta().text();
            }
            if (event.has_subagent_started()) {
                saw_started = true;
                started_child_id = event.subagent_started().child_session_id();
                // The obligation: attach to the child on its own stream, from
                // another thread so the parent's stream keeps pumping.
                fixture.attach_threads.emplace_back(
                    [&client = *fixture.client, observations, started_child_id] {
                        client.Attach(started_child_id,
                                      [observations](const v1::ServerEvent& child_event) {
                                          {
                                              std::lock_guard<std::mutex> lock(observations->mu);
                                              observations->session_id = child_event.session_id();
                                              if (child_event.has_stream_event() &&
                                                  child_event.stream_event().has_text_delta()) {
                                                  observations->text +=
                                                      child_event.stream_event()
                                                          .text_delta()
                                                          .text();
                                              }
                                              if (child_event.has_turn_completed()) {
                                                  observations->turn_completed = true;
                                              }
                                          }
                                          return !child_event.has_turn_completed();
                                      });
                    });
            }
            return true;
        });

    REQUIRE(status.ok());
    CHECK(saw_started);
    CHECK_FALSE(started_child_id.empty());
    CHECK(parent_text.find("all done") != std::string::npos);

    {
        std::lock_guard<std::mutex> lock(observations->mu);
        CHECK(observations->session_id == started_child_id);
        CHECK(observations->turn_completed);  // the child's stream delivered it
        CHECK(observations->text.find("child found it") != std::string::npos);
    }

    // The child's final text crossed into the parent's transcript as the spawn
    // tool's result.
    const auto page = fixture.db->GetMessagesPage(created.session_id, std::nullopt, 50);
    bool saw_child_result = false;
    for (const auto& stored : page.messages) {
        for (const auto& content : stored.message.content) {
            if (const auto* result = std::get_if<llm::ToolResultBlock>(&content)) {
                if (result->tool_use_id == "call-spawn" && TextOf(*result) == "child found it") {
                    saw_child_result = true;
                    CHECK_FALSE(result->is_error);
                }
            }
        }
    }
    CHECK(saw_child_result);
}

TEST_CASE("a client attaches to a non-blocking subagent whose work outlives the turn",
          "[subagent][e2e]") {
    SubagentE2EFixture fixture;
    fixture.Start();
    auto* parent_script = fixture.scripts["parent-script"].get();
    auto* child_script = fixture.scripts["child-script"].get();

    parent_script->Push(testing::ToolUseResponse(
        "call-spawn", "spawn_subagent",
        {{"task", "work in the background"}, {"blocking", false}, {"agent_preset", "child"}}));
    parent_script->Push(testing::TextResponse("parent out"),
                        TextStream("parent out", llm::StopReason::EndTurn));
    child_script->Push(testing::TextResponse("late result"),
                       TextStream("late result", llm::StopReason::EndTurn));

    client::SessionClient::CreateOptions options;
    options.project_root = (fixture.dir / "work").string();
    options.agent_preset = "main";
    options.run_mode = v1::PERMISSION_RUN_MODE_ALLOW_ONLY;
    const auto created = fixture.client->CreateSession(options);
    REQUIRE(created.status.ok());

    auto observations = std::make_shared<ChildObservations>();
    std::string started_child_id;

    const auto status = fixture.client->RunTurn(
        created.session_id, "go", [&](const v1::ServerEvent& event) {
            if (event.has_subagent_started()) {
                started_child_id = event.subagent_started().child_session_id();
                fixture.attach_threads.emplace_back(
                    [&client = *fixture.client, observations, started_child_id] {
                        client.Attach(started_child_id,
                                      [observations](const v1::ServerEvent& child_event) {
                                          {
                                              std::lock_guard<std::mutex> lock(observations->mu);
                                              if (child_event.has_turn_completed()) {
                                                  observations->turn_completed = true;
                                              }
                                          }
                                          return !child_event.has_turn_completed();
                                      });
                    });
            }
            return true;
        });

    REQUIRE(status.ok());
    REQUIRE_FALSE(started_child_id.empty());

    // The parent's turn is over; the attach thread keeps reading until the
    // child finishes. Joining here is the assertion that the child completed
    // on its own stream, attached -- had nobody attached, the initial-attach
    // guard would have terminated it instead.
    for (auto& thread : fixture.attach_threads) {
        if (thread.joinable()) thread.join();
    }
    fixture.attach_threads.clear();

    {
        std::lock_guard<std::mutex> lock(observations->mu);
        CHECK(observations->turn_completed);
    }
    const auto child_row = fixture.db->GetSession(started_child_id);
    REQUIRE(child_row.has_value());
    CHECK(child_row->parent_session_id == created.session_id);
}
