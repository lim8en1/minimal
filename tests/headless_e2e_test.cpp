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
#include <optional>
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

// A FakeLlmProvider that counts CALLS, release-stored before the scripted step
// is consumed. The store-then-consume order is what lets a test observe "the
// first attempt has failed" and know that anything the turn's thread published
// before the call (the cancel handle RunTurn populates) is visible to it.
class CountingFakeProvider : public testing::FakeLlmProvider {
public:
    explicit CountingFakeProvider(std::shared_ptr<std::atomic<int>> calls)
        : calls_(std::move(calls)) {}

    llm::ChatResponse Complete(const llm::ChatRequest& request) override {
        calls_->fetch_add(1, std::memory_order_release);
        return FakeLlmProvider::Complete(request);
    }

    llm::ChatResponse StreamComplete(const llm::ChatRequest& request,
                                     const llm::StreamEventSink& sink) override {
        calls_->fetch_add(1, std::memory_order_release);
        return FakeLlmProvider::StreamComplete(request, sink);
    }

private:
    std::shared_ptr<std::atomic<int>> calls_;
};

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
        steps.push_back({std::move(response), std::move(events), std::nullopt});
    }

    // A retryable provider failure: the agent loop parks in retry backoff
    // before reaching for the next scripted step.
    void ScriptError(llm::LlmError error) {
        steps.push_back({llm::ChatResponse{}, {}, std::move(error)});
    }

    // Incremented once per provider CALL (not per provider instance), so a
    // test can rendezvous with a specific attempt -- e.g. "the first attempt
    // has failed, the server is now parked in backoff".
    std::shared_ptr<std::atomic<int>> provider_attempts =
        std::make_shared<std::atomic<int>>(0);

    void Start() {
        presets = std::make_unique<agent::AgentPresetRegistry>(
            dir, skills, config::LoadSettingsConfig(dir / "settings.json"));
        auto scripted = std::make_shared<decltype(steps)>(steps);
        auto attempts = provider_attempts;
        providers.Register("fake", [scripted, attempts](const llm::ProviderConfig&) {
            auto provider = std::make_unique<CountingFakeProvider>(attempts);
            for (const auto& step : *scripted) {
                if (step.error) {
                    provider->PushError(*step.error);
                } else {
                    provider->PushResponse(step.response, step.events);
                }
            }
            return provider;
        });
        observers = std::make_unique<agent::SessionObserverRegistry>(common::DefaultLogger());
        db = std::make_unique<session::SessionDatabase>(dir / "sessions.db");
        sessions = std::make_unique<agent::SessionRegistry>(*presets, tools, providers, *observers,
                                                            *db, agent::LoopConfig{},
                                                            common::DefaultLogger());
        service = std::make_unique<rpc::SessionServiceImpl>(*sessions, *db,
                                                             config::SettingsConfig{},
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
        std::optional<llm::LlmError> error;
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

TEST_CASE("a client-side cancel ends the turn with STOP_REASON_CANCELLED",
          "[headless][cancel]") {
    // The wire path the headless client's SIGINT handling depends on: RunTurn's
    // handle writes a CancelTurn ClientEvent on the live stream, and the server
    // ends the turn with its own stop reason instead of dropping the stream.
    // The scripted first step fails retryably, so the cancel lands while the
    // server is parked in retry backoff (500 ms base) -- the same window the
    // manual exit-4 scenario drives against a real backend.
    HeadlessFixture fixture;
    fixture.ScriptError(llm::LlmError(llm::LlmErrorCategory::Transport, "connection refused"));
    fixture.Script(testing::TextResponse("never delivered"),
                   TextStream("never delivered", llm::StopReason::EndTurn));
    fixture.Start();

    client::SessionClient::CreateOptions options;
    options.project_root = (fixture.dir / "work").string();
    options.agent_preset = "main";
    const auto created = fixture.client->CreateSession(options);
    REQUIRE(created.status.ok());

    // RunTurn blocks, so it runs on a worker; the test thread rendezvous on
    // the provider's first call -- by then the handle RunTurn populated before
    // reading any event is visible (release/acquire through the counter), and
    // the server is inside the backoff window.
    std::shared_ptr<client::SessionClient::TurnHandle> handle;
    v1::StopReason stop = v1::STOP_REASON_UNSPECIFIED;
    std::string text;
    grpc::Status status;
    std::thread turn_thread([&] {
        status = fixture.client->RunTurn(
            created.session_id, "hello",
            [&](const v1::ServerEvent& event) {
                if (event.has_stream_event() && event.stream_event().has_text_delta()) {
                    text += event.stream_event().text_delta().text();
                }
                if (event.has_turn_completed()) {
                    stop = event.turn_completed().stop_reason();
                }
                return true;
            },
            &handle);
    });

    while (fixture.provider_attempts->load(std::memory_order_acquire) < 1) {
        std::this_thread::sleep_for(2ms);
    }
    REQUIRE(handle != nullptr);
    handle->Cancel();

    turn_thread.join();
    CHECK(status.ok());
    CHECK(stop == v1::STOP_REASON_CANCELLED);
    CHECK_FALSE(fixture.provider_attempts->load() >= 2);  // no second attempt ran
    CHECK(text.empty());                                  // its answer never arrived
}
