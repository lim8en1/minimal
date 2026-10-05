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
#include "minimal/rpc/session_wiring.hpp"
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
        std::filesystem::create_directories(dir / "prompts");
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
        Write("prompts/subagent.md", "You are running as a subagent.\n");

        scripts["parent-script"] = std::make_shared<testing::ScriptedProviderScript>();
        scripts["child-script"] = std::make_shared<testing::ScriptedProviderScript>();
    }

    void Write(const std::filesystem::path& relative, const std::string& contents) {
        std::ofstream out(dir / relative);
        out << contents;
    }

    void Start() {
        presets = std::make_unique<agent::AgentPresetRegistry>(
            dir, config::LoadSettingsConfig(dir / "settings.json"));
        providers.Register("fake", [scripts = this->scripts](const llm::ProviderConfig& cfg)
                                       -> std::unique_ptr<llm::LlmProvider> {
            const auto it = scripts.find(cfg.model_name);
            if (it == scripts.end()) return nullptr;
            return std::make_unique<testing::ScriptedProvider>(it->second);
        });
        observers = std::make_unique<agent::SessionObserverRegistry>(common::DefaultLogger());
        db = std::make_unique<session::SessionDatabase>(dir / "sessions.db");
        sessions = std::make_unique<agent::SessionRegistry>(*presets, rpc::FinishSessionWiring,
            plugins::HostRegistries{.tools = &tools,
                                    .providers = &providers,
                                    .observers = observers.get()},
            *db, agent::LoopConfig{}, common::DefaultLogger());
        manager = std::make_unique<agent::SubagentManager>(*sessions, *db, *presets, 4,
                                                           /*default_subagent_model=*/"",
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
                                                            config::PermissionsConfig{},
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
    bool session_ended = false;
};

// A tool that runs until cancelled -- the child parks here, so the test's
// cancel is what ends it, exactly like a real long-running subagent task.
class SlowTool : public tools::Tool {
public:
    SlowTool() {
        spec_.name = "slow";
        spec_.category = tools::ToolCategory::ReadOnly;
        spec_.definition.name = spec_.name;
        spec_.definition.description = "runs until cancelled";
        spec_.definition.input_schema = nlohmann::json::object();
    }
    const tools::ToolSpec& spec() const override { return spec_; }
    tools::ToolResult execute(const nlohmann::json&, tools::ToolContext& ctx) override {
        while (!(ctx.is_cancelled && ctx.is_cancelled())) {
            std::this_thread::sleep_for(10ms);
        }
        return tools::ToolResult{true, "[cancelled]"};
    }

private:
    tools::ToolSpec spec_;
};

// The child's turn must not COMPLETE before the client's attach lands. Once a
// session's broadcaster closes, a late subscriber is handed an empty, closed
// queue and observes nothing at all -- and the scripted child below finishes
// in about a millisecond, which lost that race in ~30% of runs on Linux (the
// attach saw no events, so the child's session id never arrived). Parking the
// child's turn in a tool until the attach reports its first event makes the
// attach provably live while the child still has events to deliver. The cap
// keeps a genuine attach failure a FAILED assertion rather than a hang.
class AttachGateTool : public tools::Tool {
public:
    explicit AttachGateTool(std::shared_ptr<ChildObservations> observations)
        : observations_(std::move(observations)) {
        spec_.name = "gate";
        spec_.category = tools::ToolCategory::ReadOnly;
        spec_.definition.name = spec_.name;
        spec_.definition.description = "parks until the client's attach is live";
        spec_.definition.input_schema = nlohmann::json::object();
    }
    const tools::ToolSpec& spec() const override { return spec_; }
    tools::ToolResult execute(const nlohmann::json&, tools::ToolContext&) override {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::lock_guard<std::mutex> lock(observations_->mu);
                if (!observations_->session_id.empty()) break;
            }
            std::this_thread::sleep_for(5ms);
        }
        return tools::ToolResult{false, "gate opened"};
    }

private:
    std::shared_ptr<ChildObservations> observations_;
    tools::ToolSpec spec_;
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
    // "gate", not an instant tool: the child must still be running when the
    // attach below lands -- see AttachGateTool.
    child_script->Push(testing::ToolUseResponse("r1", "gate", nlohmann::json::object()));
    child_script->Push(testing::TextResponse("child found it"),
                       TextStream("child found it", llm::StopReason::EndTurn));

    client::SessionClient::CreateOptions options;
    options.project_root = (fixture.dir / "work").string();
    options.agent_preset = "main";
    options.run_mode = v1::PERMISSION_RUN_MODE_ALLOW_ONLY;
    const auto created = fixture.client->CreateSession(options);
    REQUIRE(created.status.ok());

    auto observations = std::make_shared<ChildObservations>();
    fixture.tools.Register(std::make_unique<AttachGateTool>(observations));
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

    // Before reading what the attach saw, as the other two attach cases here
    // do: the thread was only joined by the fixture's destructor, so these
    // assertions raced the very stream they are about. The gate above bounds
    // the wait -- a child that ended before the attach cannot park it.
    for (auto& thread : fixture.attach_threads) {
        if (thread.joinable()) thread.join();
    }
    fixture.attach_threads.clear();

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

TEST_CASE("a cancelled blocking subagent delivers SessionEnded to its attached client",
          "[subagent][e2e][invariant]") {
    // THE TERMINAL EVENT: a session terminated server-side while a client was
    // attached -- here, a blocking child killed by a cancelled spawn -- used to
    // go silent. The attach thread pumped a stream that would never produce
    // again and could not distinguish "killed" from "finished quietly".
    // Terminate now emits SessionEndedEvent before closing the queue, so the
    // writer drains it and the attach loop ends.
    SubagentE2EFixture fixture;
    fixture.tools.Register(std::make_unique<SlowTool>());
    fixture.Start();
    auto* parent_script = fixture.scripts["parent-script"].get();
    auto* child_script = fixture.scripts["child-script"].get();

    parent_script->Push(testing::ToolUseResponse(
        "call-spawn", "spawn_subagent", {{"task", "work for a while"}, {"agent_preset", "child"}}));
    parent_script->Push(testing::TextResponse("parent out"),
                        TextStream("parent out", llm::StopReason::EndTurn));
    // The child streams one event (proof its attach stream is live), then parks
    // in the slow tool until the cancel lands.
    child_script->Push(testing::ToolUseResponse("r1", "slow"),
                       TextStream("child running", llm::StopReason::ToolUse));

    client::SessionClient::CreateOptions options;
    options.project_root = (fixture.dir / "work").string();
    options.agent_preset = "main";
    options.run_mode = v1::PERMISSION_RUN_MODE_ALLOW_ONLY;
    const auto created = fixture.client->CreateSession(options);
    REQUIRE(created.status.ok());

    auto observations = std::make_shared<ChildObservations>();
    auto child_stream_live = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<client::SessionClient::TurnHandle> turn_handle;

    const auto status = fixture.client->RunTurn(
        created.session_id, "go",
        [&](const v1::ServerEvent& event) {
            if (event.has_subagent_started()) {
                const std::string child_id = event.subagent_started().child_session_id();
                fixture.attach_threads.emplace_back(
                    [&client = *fixture.client, observations, child_stream_live, child_id] {
                        client.Attach(child_id,
                                      [observations, child_stream_live](const v1::ServerEvent&
                                                                            child_event) {
                                          {
                                              std::lock_guard<std::mutex> lock(observations->mu);
                                              observations->session_id = child_event.session_id();
                                              if (child_event.has_stream_event() &&
                                                  child_event.stream_event().has_text_delta()) {
                                                  // An event delivered through the writer
                                                  // proves the stream is attached.
                                                  child_stream_live->store(true);
                                              }
                                              if (child_event.has_turn_completed()) {
                                                  observations->turn_completed = true;
                                              }
                                              if (child_event.has_session_ended()) {
                                                  observations->session_ended = true;
                                              }
                                          }
                                          return !(child_event.has_turn_completed() ||
                                                   child_event.has_session_ended());
                                      });
                    });
                // Rendezvous ON THIS THREAD (the RunTurn read loop): wait until
                // the child's attach stream has actually delivered an event,
                // then cancel the parent -- which propagates into the child as
                // a terminate. Cancelling from the main thread after RunTurn
                // returned would never run: RunTurn returns only when the turn
                // has ended, and this turn ends only by being cancelled.
                const auto attached = std::chrono::steady_clock::now() + 5s;
                while (!child_stream_live->load() &&
                       std::chrono::steady_clock::now() < attached) {
                    std::this_thread::sleep_for(5ms);
                }
                REQUIRE(child_stream_live->load());
                REQUIRE(turn_handle != nullptr);
                turn_handle->Cancel();
            }
            return true;
        },
        [&turn_handle](const std::shared_ptr<client::SessionClient::TurnHandle>& handle) {
            turn_handle = handle;
        });

    REQUIRE(status.ok());
    for (auto& thread : fixture.attach_threads) {
        if (thread.joinable()) thread.join();
    }
    fixture.attach_threads.clear();

    {
        std::lock_guard<std::mutex> lock(observations->mu);
        CHECK(observations->session_ended);  // the kill is on the wire, not silence
        CHECK_FALSE(observations->turn_completed);  // the child never finished its turn
    }
}
