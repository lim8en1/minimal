// MCP end-to-end over a real gRPC channel on a unix socket -- the fourth test
// tier applied to track G. A real SessionServiceImpl, a real Session worker,
// the real McpClientManager spawning a real stdio MCP subprocess, and the
// per-server permission chain answered by a raw stub client mid-stream.
#include <atomic>
#include <cstdio>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace std::chrono_literals;

#include <catch2/catch_test_macros.hpp>
#include <grpcpp/create_channel.h>

#include "fakes.hpp"
#include "temp_dir.hpp"
#include "minimal/config/mcps_config.hpp"
#include "minimal/mcp/mcp_client_manager.hpp"
#include "minimal/rpc/server.hpp"
#include "minimal/rpc/session_service.hpp"
#include "minimal/v1/session_service.grpc.pb.h"

using namespace minimal;

namespace {

constexpr const char* kFakeServer = MINIMAL_MCP_FAKE_SERVER;

struct McpE2eFixture {
    McpE2eFixture(std::chrono::milliseconds request_timeout = 5s) {
        dir = tempdir.path();
        std::filesystem::create_directories(dir / "agents");
        std::filesystem::create_directories(dir / "work");

        Write("providers.json",
              nlohmann::json{{"providers",
                              {{"main",
                                {{"backend", "fake"},
                                 {"url", "http://localhost/v1"},
                                 {"models", {{"test-model", nlohmann::json::object()}}}}}}}}
                  .dump());
        Write("settings.json", nlohmann::json{{"default_agent_preset", "main"}}.dump());
        Write("permissions.json", "{}");  // mcp_default_mode defaults to ASK
        Write("agents/main.md",
              "---\nname: main\nmodel: main/test-model\nallowed_tools: [\"*\"]\n"
              "allowed_mcp_servers: [\"fake\"]\n---\nYou help.\n");
        skills = config::SkillRegistry::Load(dir);

        timeouts.request = request_timeout;
    }

    void Write(const std::filesystem::path& relative, const std::string& contents) const {
        const auto path = dir / relative;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path) << contents;
    }

    void Script(llm::ChatResponse response, std::vector<llm::StreamEvent> events = {}) {
        if (events.empty()) {
            events = {llm::StreamEvent{llm::MessageStartEvent{"m-1", "test-model"}},
                      llm::StreamEvent{llm::MessageStopEvent{}}};
        }
        script->Push(std::move(response), std::move(events));
    }

    void Start() {
        presets = std::make_unique<agent::AgentPresetRegistry>(dir, skills);
        auto script_copy = script;
        providers.Register("fake", [script_copy](const llm::ProviderConfig&) {
            return std::make_unique<testing::ScriptedProvider>(script_copy);
        });
        observers = std::make_unique<agent::SessionObserverRegistry>(common::DefaultLogger());
        db = std::make_unique<session::SessionDatabase>(dir / "sessions.db");
        sessions = std::make_unique<agent::SessionRegistry>(*presets, tools, providers, *observers,
                                                            *db, agent::LoopConfig{},
                                                            common::DefaultLogger());

        nlohmann::json entry = {{"name", "fake"},
                                {"transport", "stdio"},
                                {"command", kFakeServer},
                                {"args", {"normal"}},
                                {"enabled", true}};
        auto mcps = config::ParseMcpsConfig(nlohmann::json::array({entry}));
        manager = std::make_unique<mcp::McpClientManager>(std::move(mcps), tools, timeouts,
                                                          common::DefaultLogger());

        service = std::make_unique<rpc::SessionServiceImpl>(*sessions, *db,
                                                            config::SettingsConfig{},
                                                            config::PermissionsConfig{},
                                                            common::DefaultLogger());
        service->set_mcp_manager(manager.get());

        socket = dir / "test.sock";
        server = std::make_unique<rpc::Server>(*service, socket);
        REQUIRE(server->listening());

        channel = grpc::CreateChannel(rpc::SocketTarget(socket), grpc::InsecureChannelCredentials());
        stub = v1::SessionService::NewStub(channel);
    }

    std::string CreateSession() {
        grpc::ClientContext context;
        v1::CreateSessionRequest request;
        request.set_project_root((dir / "work").string());
        request.set_agent_preset("main");
        v1::CreateSessionResponse response;
        const auto status = stub->CreateSession(&context, request, &response);
        REQUIRE(status.ok());
        return response.session().session_id();
    }

    ~McpE2eFixture() {
        stub.reset();
        channel.reset();
        if (server) server->Shutdown();
        server.reset();
        if (sessions) sessions->TerminateAll();
    }

    testing::TempDir tempdir;
    std::filesystem::path dir, socket;
    config::SkillRegistry skills;
    tools::ToolRegistry tools;
    llm::ProviderFactoryRegistry providers;
    std::shared_ptr<testing::ScriptedProviderScript> script =
        std::make_shared<testing::ScriptedProviderScript>();
    std::unique_ptr<agent::AgentPresetRegistry> presets;
    std::unique_ptr<agent::SessionObserverRegistry> observers;
    std::unique_ptr<session::SessionDatabase> db;
    std::unique_ptr<agent::SessionRegistry> sessions;
    std::unique_ptr<mcp::McpClientManager> manager;
    mcp::McpLimits timeouts{5s, 5s};
    std::unique_ptr<rpc::SessionServiceImpl> service;
    std::unique_ptr<rpc::Server> server;
    std::shared_ptr<grpc::Channel> channel;
    std::unique_ptr<v1::SessionService::Stub> stub;
};

v1::ClientEvent ChatTurn(const std::string& session_id, const std::string& text) {
    v1::ClientEvent event;
    event.set_session_id(session_id);
    event.mutable_chat_turn()->set_text(text);
    return event;
}

v1::ClientEvent PermissionAnswer(const std::string& session_id, const std::string& request_id,
                                 v1::PermissionDecision decision) {
    v1::ClientEvent event;
    event.set_session_id(session_id);
    auto* response = event.mutable_permission_response();
    response->set_request_id(request_id);
    response->set_decision(decision);
    return event;
}

template <typename Stream>
grpc::Status DrainAndFinish(Stream& stream) {
    v1::ServerEvent event;
    while (stream->Read(&event)) {
    }
    stream->WritesDone();
    return stream->Finish();
}

}  // namespace

TEST_CASE("tools from a stdio MCP server appear as mcp__server__tool and execute", "[mcp_e2e]") {
    McpE2eFixture fixture;
    fixture.Script(testing::ToolUseResponse("call-1", "mcp__fake__echo", {{"text", "hello mcp"}}));
    fixture.Script(testing::TextResponse("echoed"));
    fixture.Start();

    // Nothing has spawned yet: liveness and discovery are per-session, not per-boot.
    CHECK_FALSE(fixture.manager->StatusOf("fake").alive);

    const std::string session_id = fixture.CreateSession();
    grpc::ClientContext context;
    auto stream = fixture.stub->OpenSession(&context);
    REQUIRE(stream->Write(ChatTurn(session_id, "call the echo tool")));

    bool saw_started = false;
    bool saw_finished = false;
    bool saw_turn_completed = false;
    v1::ServerEvent event;
    while (stream->Read(&event)) {
        if (event.has_permission_request()) {
            // MCP tools are Ask by default (mcp_default_mode): the first call
            // must prompt, and the answer must unblock the worker.
            REQUIRE(stream->Write(PermissionAnswer(
                session_id, event.permission_request().request_id(),
                v1::PERMISSION_DECISION_ALLOW)));
        }
        if (event.has_tool_execution()) {
            if (event.tool_execution().started() &&
                event.tool_execution().tool_name() == "mcp__fake__echo") {
                saw_started = true;
            }
            if (!event.tool_execution().started() && !event.tool_execution().is_error()) {
                saw_finished = true;
            }
        }
        if (event.has_turn_completed()) {
            saw_turn_completed = true;
            break;  // TurnCompleted is the end-of-turn signal; the stream stays open
        }
    }
    stream->WritesDone();
    CHECK(stream->Finish().ok());

    CHECK(saw_started);
    CHECK(saw_finished);
    CHECK(saw_turn_completed);
    CHECK(fixture.manager->StatusOf("fake").alive);  // connected on session start
}

TEST_CASE("per-server MCP permission policy round-trips over the wire", "[mcp_e2e]") {
    McpE2eFixture fixture;  // permissions.json leaves mcp_default_mode = ASK
    fixture.Script(testing::ToolUseResponse("call-1", "mcp__fake__echo", {{"text", "hi"}}));
    fixture.Script(testing::TextResponse("done"));
    fixture.Start();

    const std::string session_id = fixture.CreateSession();
    grpc::ClientContext context;
    auto stream = fixture.stub->OpenSession(&context);
    REQUIRE(stream->Write(ChatTurn(session_id, "call the echo tool")));

    bool answered = false;
    bool saw_turn_completed = false;
    v1::ServerEvent event;
    while (stream->Read(&event)) {
        if (event.has_permission_request()) {
            const auto& request = event.permission_request();
            CHECK(request.tool_name() == "mcp__fake__echo");
            // MCP tools disclose the default (name only, no grants): the
            // consent unit is the SERVER, per McpPermissionPolicy.
            CHECK(request.grants_size() == 0);
            CHECK(request.complete());
            const bool wrote = stream->Write(
                PermissionAnswer(session_id, request.request_id(), v1::PERMISSION_DECISION_ALLOW));
            std::fprintf(stderr, "CLIENT-ANSWER wrote=%d\n", wrote);
            answered = true;
        }
        if (event.has_turn_completed()) {
            saw_turn_completed = true;
            break;
        }
    }
    stream->WritesDone();
    CHECK(stream->Finish().ok());

    CHECK(answered);
    CHECK(saw_turn_completed);
}

TEST_CASE("a denied MCP call is an error result, not a hang", "[mcp_e2e]") {
    McpE2eFixture fixture;
    fixture.Script(testing::ToolUseResponse("call-1", "mcp__fake__echo", {{"text", "hi"}}));
    fixture.Script(testing::TextResponse("understood the refusal"));
    fixture.Start();

    const std::string session_id = fixture.CreateSession();
    grpc::ClientContext context;
    auto stream = fixture.stub->OpenSession(&context);
    REQUIRE(stream->Write(ChatTurn(session_id, "call the echo tool")));

    bool saw_finished_error = false;
    bool saw_turn_completed = false;
    v1::ServerEvent event;
    while (stream->Read(&event)) {
        if (event.has_permission_request()) {
            REQUIRE(stream->Write(PermissionAnswer(session_id,
                                                   event.permission_request().request_id(),
                                                   v1::PERMISSION_DECISION_DENY)));
        }
        if (event.has_tool_execution() && !event.tool_execution().started() &&
            event.tool_execution().is_error()) {
            saw_finished_error = true;
        }
        if (event.has_turn_completed()) {
            saw_turn_completed = true;
            break;
        }
    }
    stream->WritesDone();
    CHECK(stream->Finish().ok());

    CHECK(saw_finished_error);
    CHECK(saw_turn_completed);
}

TEST_CASE("a hanging MCP tool degrades to a bounded error over the wire", "[mcp_e2e]") {
    // Request timeout 300 ms against the fake server's hang tool (10 min).
    McpE2eFixture fixture(300ms);
    fixture.Script(testing::ToolUseResponse("call-1", "mcp__fake__hang", nlohmann::json::object()));
    fixture.Script(testing::TextResponse("moving on after the failure"));
    fixture.Start();

    const std::string session_id = fixture.CreateSession();
    grpc::ClientContext context;
    auto stream = fixture.stub->OpenSession(&context);
    REQUIRE(stream->Write(ChatTurn(session_id, "call the hang tool")));

    bool saw_finished_error = false;
    bool saw_turn_completed = false;
    v1::ServerEvent event;
    while (stream->Read(&event)) {
        if (event.has_permission_request()) {
            REQUIRE(stream->Write(PermissionAnswer(
                session_id, event.permission_request().request_id(),
                v1::PERMISSION_DECISION_ALLOW)));
        }
        if (event.has_tool_execution() && !event.tool_execution().started()) {
            saw_finished_error = event.tool_execution().is_error();
        }
        if (event.has_turn_completed()) {
            saw_turn_completed = true;
            break;  // end-of-turn signal; the stream stays open across it
        }
    }
    stream->WritesDone();
    CHECK(stream->Finish().ok());

    CHECK(saw_finished_error);
    CHECK(saw_turn_completed);
}
