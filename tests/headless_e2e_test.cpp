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

#include <unistd.h>

#include <catch2/catch_test_macros.hpp>

#include "minimal/client/utf8.hpp"

#include "fakes.hpp"
#include "temp_dir.hpp"
#include "minimal/client/session_client.hpp"
#include "minimal/rpc/server.hpp"
#include "minimal/rpc/session_service.hpp"

using namespace minimal;
using namespace std::chrono_literals;

namespace {

struct HeadlessFixture {
    HeadlessFixture() {
        // A unique arena per process: catch_discover_tests runs every TEST_CASE
        // as its own ctest entry, so a fixed path would be wiped out from under
        // a concurrent run of this very file (ctest -j).
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
        // execute is Ask, so AllowOnly has something to auto-deny.
        Write("permissions.json", nlohmann::json{{"fallback", {{"execute", "ask"}}}}.dump());
        Write("agents/main.md",
              "---\nname: main\nmodel: main/test-model\nallowed_tools: [\"*\"]\n---\nYou help.\n");
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
    }

    struct Step {
        llm::ChatResponse response;
        std::vector<llm::StreamEvent> events;
    };

    testing::TempDir tempdir;
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

TEST_CASE("a stored session resumes headlessly", "[headless][resume]") {
    // The whole point of the B gap fix, exercised through the real client: a
    // transcript survives the server-side termination of its session and
    // becomes the context of the next one.
    HeadlessFixture fixture;
    // Both incarnations build their providers from this snapshot, so each
    // consumes one step -- script them identical.
    fixture.Script(testing::TextResponse("the answer is 42"),
                   TextStream("the answer is 42", llm::StopReason::EndTurn));
    fixture.Script(testing::TextResponse("the answer is 42"),
                   TextStream("the answer is 42", llm::StopReason::EndTurn));
    fixture.Start();

    client::SessionClient::CreateOptions options;
    options.project_root = (fixture.dir / "work").string();
    options.agent_preset = "main";
    options.run_mode = v1::PERMISSION_RUN_MODE_ALLOW_ONLY;
    const auto created = fixture.client->CreateSession(options);
    REQUIRE(created.status.ok());

    std::string first_text;
    REQUIRE(fixture.client
                ->RunTurn(created.session_id, "what is the answer?",
                          [&](const v1::ServerEvent& event) {
                              if (event.has_stream_event() &&
                                  event.stream_event().has_text_delta()) {
                                  first_text += event.stream_event().text_delta().text();
                              }
                              return true;
                          })
                .ok());
    CHECK(first_text == "the answer is 42");

    // RunTurn returned, so the stream closed and the server terminated the
    // session. Resume it rather than creating a new one.
    client::SessionClient::CreateOptions resume;
    resume.resume_session_id = created.session_id;
    resume.run_mode = v1::PERMISSION_RUN_MODE_ALLOW_ONLY;
    const auto resumed = fixture.client->CreateSession(resume);
    REQUIRE(resumed.status.ok());
    CHECK(resumed.session_id == created.session_id);

    std::string second_text;
    REQUIRE(fixture.client
                ->RunTurn(resumed.session_id, "and again?",
                          [&](const v1::ServerEvent& event) {
                              if (event.has_stream_event() &&
                                  event.stream_event().has_text_delta()) {
                                  second_text += event.stream_event().text_delta().text();
                              }
                              return true;
                          })
                .ok());
    CHECK(second_text == "the answer is 42");

    // One transcript across both incarnations: the stored turn, then the resumed
    // turn, contiguous and unduplicated.
    const auto loaded = fixture.db->LoadMessages(created.session_id);
    REQUIRE(loaded.size() == 4);
    CHECK(std::get<llm::TextBlock>(loaded[0].message.content.front()).text ==
          "what is the answer?");
    CHECK(std::get<llm::TextBlock>(loaded[1].message.content.front()).text ==
          "the answer is 42");
    CHECK(std::get<llm::TextBlock>(loaded[2].message.content.front()).text == "and again?");
    CHECK(std::get<llm::TextBlock>(loaded[3].message.content.front()).text ==
          "the answer is 42");
}

TEST_CASE("truncating a name at a byte limit never splits a UTF-8 character",
          "[client][utf8]") {
    // CreateSessionRequest.name is a proto3 string: the SERVER's parser rejects
    // invalid UTF-8 while the client's serializer only logs, so a plain
    // substr(0, 60) turned any non-ASCII prompt whose 60th byte landed inside a
    // multi-byte sequence into an opaque "could not create a session" error.
    // Same helper, exercised here because it is pure.
    const std::string emoji = "\xf0\x9f\x99\x82";  // 4 bytes
    std::string prompt;
    for (int i = 0; i < 20; ++i) prompt += emoji;  // 80 bytes, no ASCII

    const auto truncated = minimal::client::TruncateUtf8(prompt, 60);
    CHECK(truncated.size() == 60);  // 15 whole emoji
    // Every byte accounted for as a complete sequence: re-scanning finds no
    // truncated lead byte at the end.
    std::size_t i = 0;
    while (i < truncated.size()) {
        const auto c = static_cast<unsigned char>(truncated[i]);
        const std::size_t len = (c < 0x80) ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : 4;
        REQUIRE(i + len <= truncated.size());
        i += len;
    }

    // A limit that lands mid-character drops that character rather than half of it.
    CHECK(minimal::client::TruncateUtf8(emoji + emoji, 6).size() == 4);
    CHECK(minimal::client::TruncateUtf8("ascii", 60) == "ascii");
}
