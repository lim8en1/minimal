// The headless client against a real server over a unix socket.
//
// This is the tier ARCHITECTURE.md calls "server plus the headless -p client
// over a real gRPC channel", and the reason -p was built before the TUI: it is
// the smallest real client, so it is what proves the proto is a genuine boundary
// rather than a TUI-shaped hole.
//
// The SessionClient here is the same one the binary uses. What is not exercised
// is argv parsing and stdout formatting, which the manual run in the milestone's
// verification steps covers.

#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "fakes.hpp"
#include "minimal/client/session_client.hpp"
#include "minimal/rpc/server.hpp"
#include "minimal/rpc/session_service.hpp"

using namespace minimal;
using namespace std::chrono_literals;

namespace {

struct HeadlessFixture {
    HeadlessFixture() {
        dir = std::filesystem::temp_directory_path() / "minimal_headless_e2e_arena";
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir / "agents");
        std::filesystem::create_directories(dir / "work");

        Write("providers.json", nlohmann::json::array({{{"name", "main"},
                                                        {"provider", "fake"},
                                                        {"base_url", "http://localhost/v1"},
                                                        {"model_name", "test-model"}}})
                                    .dump());
        Write("settings.json", nlohmann::json{{"default_agent_preset", "main"}}.dump());
        // execute is Ask, so AllowOnly has something to auto-deny.
        Write("permissions.json", nlohmann::json{{"fallback", {{"execute", "ask"}}}}.dump());
        Write("agents/main.md",
              "---\nname: main\nprovider: main\nallowed_tools: [\"*\"]\n---\nYou help.\n");
        skills = config::SkillRegistry::Load(dir);
    }

    void Write(const std::filesystem::path& relative, const std::string& contents) const {
        const auto path = dir / relative;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path) << contents;
    }

    void Script(llm::ChatResponse response, std::vector<llm::StreamEvent> events) {
        steps.push_back({std::move(response), std::move(events)});
    }

    void Start() {
        presets = std::make_unique<agent::AgentPresetRegistry>(dir, skills);
        auto scripted = std::make_shared<decltype(steps)>(steps);
        providers.Register("fake", [scripted](const llm::ProviderConfig&) {
            auto provider = std::make_unique<testing::FakeLlmProvider>();
            for (const auto& step : *scripted) provider->PushResponse(step.response, step.events);
            return provider;
        });
        observers = std::make_unique<agent::SessionObserverRegistry>(common::DefaultLogger());
        db = std::make_unique<session::SessionDatabase>(dir / "sessions.db");
        sessions = std::make_unique<agent::SessionRegistry>(*presets, tools, providers, *observers,
                                                            *db, agent::LoopConfig{},
                                                            common::DefaultLogger());
        service = std::make_unique<rpc::SessionServiceImpl>(*sessions, *db,
                                                             config::SettingsConfig{},
                                                             config::PermissionsConfig{},
                                                             common::DefaultLogger());
        socket = dir / "headless.sock";
        server = std::make_unique<rpc::Server>(*service, socket);
        REQUIRE(server->listening());
        client = std::make_unique<client::SessionClient>(rpc::SocketTarget(socket));
        REQUIRE(client->WaitForConnected(5s));
    }

    ~HeadlessFixture() {
        client.reset();
        if (server) server->Shutdown();
        server.reset();
        if (sessions) sessions->TerminateAll();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    struct Step {
        llm::ChatResponse response;
        std::vector<llm::StreamEvent> events;
    };

    std::filesystem::path dir, socket;
    config::SkillRegistry skills;
    tools::ToolRegistry tools;
    llm::ProviderFactoryRegistry providers;
    std::vector<Step> steps;
    std::unique_ptr<agent::AgentPresetRegistry> presets;
    std::unique_ptr<agent::SessionObserverRegistry> observers;
    std::unique_ptr<session::SessionDatabase> db;
    std::unique_ptr<agent::SessionRegistry> sessions;
    std::unique_ptr<rpc::SessionServiceImpl> service;
    std::unique_ptr<rpc::Server> server;
    std::unique_ptr<client::SessionClient> client;
};

std::vector<llm::StreamEvent> TextStream(const std::string& text, llm::StopReason stop) {
    return {llm::StreamEvent{llm::MessageStartEvent{"m-1", "test-model"}},
            llm::StreamEvent{llm::TextDeltaEvent{0, text}},
            llm::StreamEvent{llm::ContentBlockStopEvent{0}},
            llm::StreamEvent{llm::MessageDeltaEvent{stop}},
            llm::StreamEvent{llm::MessageStopEvent{}}};
}

}  // namespace

TEST_CASE("a headless turn returns the model's text", "[headless]") {
    HeadlessFixture fixture;
    fixture.Script(testing::TextResponse("the answer is 42"),
                   TextStream("the answer is 42", llm::StopReason::EndTurn));
    fixture.Start();

    client::SessionClient::CreateOptions options;
    options.project_root = (fixture.dir / "work").string();
    options.agent_preset = "main";
    options.run_mode = v1::PERMISSION_RUN_MODE_ALLOW_ONLY;
    const auto created = fixture.client->CreateSession(options);
    REQUIRE(created.status.ok());
    REQUIRE_FALSE(created.session_id.empty());

    std::string text;
    v1::StopReason stop = v1::STOP_REASON_UNSPECIFIED;
    const auto status =
        fixture.client->RunTurn(created.session_id, "what is the answer?",
                                [&](const v1::ServerEvent& event) {
                                    if (event.has_stream_event()) {
                                        const auto& stream = event.stream_event();
                                        if (stream.has_text_delta()) {
                                            text += stream.text_delta().text();
                                        }
                                        if (stream.has_message_delta()) {
                                            stop = stream.message_delta().stop_reason();
                                        }
                                    }
                                    return true;
                                });

    CHECK(status.ok());
    CHECK(text == "the answer is 42");
    CHECK(stop == v1::STOP_REASON_END_TURN);
}

TEST_CASE("AllowOnly auto-denies instead of blocking", "[headless]") {
    // The reason PermissionRunMode::AllowOnly exists. There is no human on this
    // stream, so an Ask-gated tool must be refused rather than prompt into the
    // void. THE ASSERTION IS THAT THIS TEST TERMINATES AT ALL -- a regression
    // here is a hang, not a wrong answer.
    HeadlessFixture fixture;
    fixture.tools.Register(
        std::make_unique<testing::FakeTool>("bash", tools::ToolCategory::Execute, "ran"));
    fixture.Script(testing::ToolUseResponse("call-1", "bash", {{"command", "ls"}}),
                   {llm::StreamEvent{llm::MessageStartEvent{"m-1", "test-model"}},
                    llm::StreamEvent{llm::MessageDeltaEvent{llm::StopReason::ToolUse}}});
    fixture.Script(testing::TextResponse("I was not allowed to run that"),
                   TextStream("I was not allowed to run that", llm::StopReason::EndTurn));
    fixture.Start();

    client::SessionClient::CreateOptions options;
    options.project_root = (fixture.dir / "work").string();
    options.agent_preset = "main";
    options.run_mode = v1::PERMISSION_RUN_MODE_ALLOW_ONLY;
    const auto created = fixture.client->CreateSession(options);
    REQUIRE(created.status.ok());

    bool prompted = false;
    bool tool_errored = false;
    std::string text;
    const auto status = fixture.client->RunTurn(
        created.session_id, "list the files", [&](const v1::ServerEvent& event) {
            if (event.has_permission_request()) prompted = true;
            if (event.has_tool_execution() && !event.tool_execution().started()) {
                tool_errored = event.tool_execution().is_error();
            }
            if (event.has_stream_event() && event.stream_event().has_text_delta()) {
                text += event.stream_event().text_delta().text();
            }
            return true;
        });

    CHECK(status.ok());
    // No prompt was ever put on the wire -- not "a prompt that was auto-answered".
    CHECK_FALSE(prompted);
    CHECK(tool_errored);
    CHECK(text == "I was not allowed to run that");
}

TEST_CASE("bypass runs an Ask-gated tool without prompting", "[headless]") {
    // --dangerously-bypass, and the honest counterpart to AllowOnly: the tool
    // runs, and still nothing is asked.
    HeadlessFixture fixture;
    fixture.tools.Register(
        std::make_unique<testing::FakeTool>("bash", tools::ToolCategory::Execute, "ran"));
    fixture.Script(testing::ToolUseResponse("call-1", "bash", {{"command", "ls"}}),
                   {llm::StreamEvent{llm::MessageStartEvent{"m-1", "test-model"}},
                    llm::StreamEvent{llm::MessageDeltaEvent{llm::StopReason::ToolUse}}});
    fixture.Script(testing::TextResponse("done"), TextStream("done", llm::StopReason::EndTurn));
    fixture.Start();

    client::SessionClient::CreateOptions options;
    options.project_root = (fixture.dir / "work").string();
    options.agent_preset = "main";
    options.run_mode = v1::PERMISSION_RUN_MODE_BYPASS;
    const auto created = fixture.client->CreateSession(options);
    REQUIRE(created.status.ok());

    bool prompted = false;
    bool tool_ran_ok = false;
    const auto status = fixture.client->RunTurn(
        created.session_id, "list the files", [&](const v1::ServerEvent& event) {
            if (event.has_permission_request()) prompted = true;
            if (event.has_tool_execution() && !event.tool_execution().started()) {
                tool_ran_ok = !event.tool_execution().is_error();
            }
            return true;
        });

    CHECK(status.ok());
    CHECK_FALSE(prompted);
    CHECK(tool_ran_ok);
}

TEST_CASE("the terminal stop reason reaches the client", "[headless]") {
    // The headless exit code is derived from this, so max_tokens has to arrive
    // as itself: collapsing it into end_turn would report a truncated answer as
    // a successful one.
    HeadlessFixture fixture;
    fixture.Script(testing::TextResponse("cut off mid-", llm::StopReason::MaxTokens),
                   TextStream("cut off mid-", llm::StopReason::MaxTokens));
    fixture.Start();

    client::SessionClient::CreateOptions options;
    options.project_root = (fixture.dir / "work").string();
    options.agent_preset = "main";
    const auto created = fixture.client->CreateSession(options);
    REQUIRE(created.status.ok());

    v1::StopReason stop = v1::STOP_REASON_UNSPECIFIED;
    fixture.client->RunTurn(created.session_id, "write an essay",
                            [&](const v1::ServerEvent& event) {
                                if (event.has_stream_event() &&
                                    event.stream_event().has_message_delta()) {
                                    stop = event.stream_event().message_delta().stop_reason();
                                }
                                return true;
                            });
    CHECK(stop == v1::STOP_REASON_MAX_TOKENS);
}

TEST_CASE("a provider failure reaches the client as an ErrorEvent", "[headless]") {
    HeadlessFixture fixture;
    fixture.Start();  // nothing scripted: the fake provider throws

    client::SessionClient::CreateOptions options;
    options.project_root = (fixture.dir / "work").string();
    options.agent_preset = "main";
    const auto created = fixture.client->CreateSession(options);
    REQUIRE(created.status.ok());

    bool saw_error = false;
    const auto status =
        fixture.client->RunTurn(created.session_id, "hello", [&](const v1::ServerEvent& event) {
            if (event.has_error()) {
                CHECK_FALSE(event.error().message().empty());
                saw_error = true;
            }
            return true;
        });
    CHECK(status.ok());  // the RPC was fine; the model call was not
    CHECK(saw_error);
}

TEST_CASE("a missing project_root is refused before a session exists", "[headless]") {
    HeadlessFixture fixture;
    fixture.Start();

    client::SessionClient::CreateOptions options;
    options.agent_preset = "main";
    const auto created = fixture.client->CreateSession(options);
    CHECK_FALSE(created.status.ok());
    CHECK(created.session_id.empty());
    CHECK(fixture.sessions->size() == 0);
}

TEST_CASE("the last client detaching really shuts the server down", "[headless]") {
    // REGRESSION. The earlier version of this wiring called the blocking
    // grpc::Server::Shutdown() straight from the last-client-detached callback,
    // which fires at the tail of OpenSession -- on the handler thread. Shutdown()
    // waits for every in-flight RPC, so it waited for the RPC calling it, and the
    // binary hung on every single run.
    //
    // The earlier test missed it because its callback only set an atomic. This
    // one calls the real RequestShutdown and asserts Wait() actually returns,
    // which is the behaviour the binary depends on.
    HeadlessFixture fixture;
    fixture.Script(testing::TextResponse("bye"), TextStream("bye", llm::StopReason::EndTurn));
    fixture.Start();

    fixture.service->set_on_last_client_detached(
        [&fixture] { fixture.server->RequestShutdown(); });

    auto waited = std::make_shared<std::atomic<bool>>(false);
    auto server = fixture.server.get();
    std::thread waiter([server, waited] {
        server->Wait();
        waited->store(true);
    });

    client::SessionClient::CreateOptions options;
    options.project_root = (fixture.dir / "work").string();
    options.agent_preset = "main";
    const auto created = fixture.client->CreateSession(options);
    REQUIRE(created.status.ok());
    fixture.client->RunTurn(created.session_id, "hi",
                            [](const v1::ServerEvent&) { return true; });

    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!waited->load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    CHECK(waited->load());
    waiter.join();
}

TEST_CASE("stopping early still drains the stream", "[headless]") {
    // A caller that stops consuming must not wedge the server: gRPC's sync API
    // blocks in Write() when the peer is not reading, so RunTurn keeps reading
    // after the callback bows out.
    HeadlessFixture fixture;
    fixture.Script(testing::TextResponse("a longer answer here"),
                   TextStream("a longer answer here", llm::StopReason::EndTurn));
    fixture.Start();

    client::SessionClient::CreateOptions options;
    options.project_root = (fixture.dir / "work").string();
    options.agent_preset = "main";
    const auto created = fixture.client->CreateSession(options);
    REQUIRE(created.status.ok());

    int seen = 0;
    const auto status = fixture.client->RunTurn(created.session_id, "hi",
                                                 [&](const v1::ServerEvent&) {
                                                     ++seen;
                                                     return false;  // stop immediately
                                                 });
    CHECK(status.ok());
    CHECK(seen == 1);

    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (fixture.sessions->size() > 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    CHECK(fixture.sessions->size() == 0);
}
