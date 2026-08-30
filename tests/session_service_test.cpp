// End-to-end over a real gRPC channel on a unix socket -- the fourth test tier
// in ARCHITECTURE.md, and the first tier this project has ever had. Everything
// below the provider is real: real service, real Session, real worker thread,
// real ToolExecutor, real permission round-trip.

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <grpcpp/create_channel.h>

#include "deadline.hpp"
#include "fakes.hpp"
#include "minimal/rpc/server.hpp"
#include "minimal/rpc/session_service.hpp"
#include "minimal/v1/session_service.grpc.pb.h"

using namespace minimal;
using namespace std::chrono_literals;

namespace {

// Each session builds its own provider through the factory, so the script is
// shared and replayed per session rather than held by one instance.
//
// Every scripted response carries stream events. That is not decoration: with a
// silent response the server writes nothing, the client blocks in Read(), and
// the server blocks in Read() waiting for the client -- a test that hangs rather
// than fails. A real provider always streams.
struct ProviderScript {
    struct Step {
        llm::ChatResponse response;
        std::vector<llm::StreamEvent> events;
    };
    std::vector<Step> steps;
};

struct ServiceFixture {
    ServiceFixture() {
        dir = std::filesystem::temp_directory_path() / "minimal_session_service_test_arena";
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir / "agents");
        std::filesystem::create_directories(dir / "work");

        Write("providers.json",
              nlohmann::json::array({{{"name", "main"},
                                      {"provider", "fake"},
                                      {"base_url", "http://localhost/v1"},
                                      {"model_name", "test-model"}}})
                  .dump());
        Write("settings.json", nlohmann::json{{"default_agent_preset", "main"}}.dump());
        Write("permissions.json", "{}");
        Write("agents/main.md", "---\nname: main\nprovider: main\nallowed_tools: [\"*\"]\n---\nYou help.\n");

        script = std::make_shared<ProviderScript>();
        skills = config::SkillRegistry::Load(dir);
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
        script->steps.push_back({std::move(response), std::move(events)});
    }

    // Built after the script and any tools, so a session sees the finished setup.
    void Start() {
        presets = std::make_unique<agent::AgentPresetRegistry>(dir, skills);
        auto script_copy = script;
        providers.Register("fake", [script_copy](const llm::ProviderConfig&) {
            auto provider = std::make_unique<testing::FakeLlmProvider>();
            for (const auto& step : script_copy->steps) {
                provider->PushResponse(step.response, step.events);
            }
            return provider;
        });
        observers = std::make_unique<agent::SessionObserverRegistry>(common::DefaultLogger());
        db = std::make_unique<session::SessionDatabase>(dir / "sessions.db");
        sessions = std::make_unique<agent::SessionRegistry>(
            *presets, tools, providers, *observers, *db, agent::LoopConfig{},
            common::DefaultLogger(), attach_guard);
        service = std::make_unique<rpc::SessionServiceImpl>(
            *sessions, *db, config::SettingsConfig{}, config::PermissionsConfig{},
            common::DefaultLogger());
        service->set_on_last_client_detached([this] { last_detach.store(true); });

        socket = dir / "test.sock";
        server = std::make_unique<rpc::Server>(*service, socket);
        REQUIRE(server->listening());

        channel = grpc::CreateChannel(rpc::SocketTarget(socket), grpc::InsecureChannelCredentials());
        stub = v1::SessionService::NewStub(channel);
    }

    ~ServiceFixture() {
        stub.reset();
        channel.reset();
        if (server) server->Shutdown();
        server.reset();
        if (sessions) sessions->TerminateAll();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
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

    std::filesystem::path dir, socket;
    config::SkillRegistry skills;
    tools::ToolRegistry tools;
    llm::ProviderFactoryRegistry providers;
    std::shared_ptr<ProviderScript> script;
    std::unique_ptr<agent::AgentPresetRegistry> presets;
    std::unique_ptr<agent::SessionObserverRegistry> observers;
    std::unique_ptr<session::SessionDatabase> db;
    std::unique_ptr<agent::SessionRegistry> sessions;
    std::unique_ptr<rpc::SessionServiceImpl> service;
    std::unique_ptr<rpc::Server> server;
    std::shared_ptr<grpc::Channel> channel;
    std::unique_ptr<v1::SessionService::Stub> stub;
    std::chrono::milliseconds attach_guard = std::chrono::seconds(10);
    std::atomic<bool> last_detach{false};
};

v1::ClientEvent ChatTurn(const std::string& session_id, const std::string& text) {
    v1::ClientEvent event;
    event.set_session_id(session_id);
    event.mutable_chat_turn()->set_text(text);
    return event;
}

// Half-closes, then READS TO END OF STREAM before finishing.
//
// Draining is not politeness, it is required. gRPC's sync API blocks in Write()
// when the peer is not reading, so a client that calls Finish() with events
// still queued deadlocks both sides: the client waits for the RPC to complete,
// while the server's writer waits for the client to read. Every one of these
// tests hung on exactly that before this helper existed.
template <typename Stream>
grpc::Status DrainAndFinish(Stream& stream) {
    stream->WritesDone();
    v1::ServerEvent discarded;
    while (stream->Read(&discarded)) {
    }
    return stream->Finish();
}

// Reads until the turn terminator. A test that wants the transcript on disk has
// to wait for the turn to finish, and TurnCompletedEvent is the only event that
// says so -- stop_reason arrives once per tool round, not once per turn.
template <typename Stream>
bool WaitForTurnCompleted(Stream& stream) {
    v1::ServerEvent event;
    while (stream.Read(&event)) {
        if (event.has_turn_completed()) return true;
        if (event.has_error()) return false;
    }
    return false;
}

}  // namespace

TEST_CASE("a turn round-trips over a real channel", "[session_service]") {
    ServiceFixture fixture;
    fixture.Script(testing::TextResponse("hello from the model"));
    fixture.Start();

    const std::string session_id = fixture.CreateSession();
    REQUIRE_FALSE(session_id.empty());

    grpc::ClientContext context;
    auto stream = fixture.stub->OpenSession(&context);
    REQUIRE(stream->Write(ChatTurn(session_id, "hi")));

    // Read until the stream's stop event; the model's text arrives as deltas
    // reassembled client-side, exactly as a real client would see it.
    v1::ServerEvent event;
    bool saw_stream_event = false;
    while (stream->Read(&event)) {
        CHECK(event.session_id() == session_id);
        if (event.has_stream_event()) saw_stream_event = true;
        if (event.has_stream_event() && event.stream_event().has_message_stop()) break;
        if (event.has_error()) break;
    }
    (void)DrainAndFinish(stream);
    CHECK(saw_stream_event);
}

TEST_CASE("an unknown session id is refused", "[session_service]") {
    ServiceFixture fixture;
    fixture.Start();

    grpc::ClientContext context;
    auto stream = fixture.stub->OpenSession(&context);
    REQUIRE(stream->Write(ChatTurn("no-such-session", "hi")));
    const auto status = DrainAndFinish(stream);
    CHECK(status.error_code() == grpc::StatusCode::NOT_FOUND);
}

TEST_CASE("CreateSession requires a project_root", "[session_service]") {
    // project_root is per-session and client-supplied: it is the boundary for
    // path confinement and bash's initial cwd, so defaulting it server-side
    // would silently point the agent at the server's own cwd.
    ServiceFixture fixture;
    fixture.Start();

    grpc::ClientContext context;
    v1::CreateSessionRequest request;
    request.set_agent_preset("main");
    v1::CreateSessionResponse response;
    const auto status = fixture.stub->CreateSession(&context, request, &response);
    CHECK(status.error_code() == grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_CASE("an unknown preset is reported with its reason", "[session_service]") {
    ServiceFixture fixture;
    fixture.Start();

    grpc::ClientContext context;
    v1::CreateSessionRequest request;
    request.set_project_root((fixture.dir / "work").string());
    request.set_agent_preset("nonexistent");
    v1::CreateSessionResponse response;
    const auto status = fixture.stub->CreateSession(&context, request, &response);
    CHECK(status.error_code() == grpc::StatusCode::FAILED_PRECONDITION);
    CHECK(status.error_message().find("nonexistent") != std::string::npos);
}

TEST_CASE("a permission prompt is answered over the wire", "[session_service]") {
    // The round-trip the bidi stream exists for: the server pauses mid-turn,
    // asks, and blocks until a human answers.
    ServiceFixture fixture;
    auto tool = std::make_unique<testing::FakeTool>("bash", tools::ToolCategory::Execute, "ran it");
    tool->set_disclosure({"run make", {{"bash:/usr/bin/make", "/usr/bin/make *"}}, true});
    fixture.tools.Register(std::move(tool));
    fixture.Script(testing::ToolUseResponse("call-1", "bash", {{"command", "make"}}));
    fixture.Script(testing::TextResponse("built"));
    fixture.Write("permissions.json", nlohmann::json{{"fallback", {{"execute", "ask"}}}}.dump());
    fixture.Start();

    const std::string session_id = fixture.CreateSession();
    grpc::ClientContext context;
    auto stream = fixture.stub->OpenSession(&context);
    REQUIRE(stream->Write(ChatTurn(session_id, "build it")));

    v1::ServerEvent event;
    bool answered = false;
    bool saw_finished_tool = false;
    while (stream->Read(&event)) {
        if (event.has_permission_request()) {
            const auto& request = event.permission_request();
            CHECK(request.tool_name() == "bash");
            CHECK(request.summary() == "run make");
            REQUIRE(request.grants_size() == 1);
            CHECK(request.grants(0).label() == "/usr/bin/make *");
            CHECK(request.complete());

            v1::ClientEvent answer;
            answer.set_session_id(session_id);
            auto* response = answer.mutable_permission_response();
            response->set_request_id(request.request_id());
            response->set_decision(v1::PERMISSION_DECISION_ALLOW);
            REQUIRE(stream->Write(answer));
            answered = true;
        }
        if (event.has_tool_execution() && !event.tool_execution().started()) {
            saw_finished_tool = true;
            break;
        }
    }
    (void)DrainAndFinish(stream);

    CHECK(answered);
    CHECK(saw_finished_tool);
}

TEST_CASE("a denial over the wire produces an error result, not a hang", "[session_service]") {
    ServiceFixture fixture;
    auto tool = std::make_unique<testing::FakeTool>("bash", tools::ToolCategory::Execute, "ran it");
    tool->set_disclosure({"run make", {{"bash:/usr/bin/make", "/usr/bin/make *"}}, true});
    fixture.tools.Register(std::move(tool));
    fixture.Script(testing::ToolUseResponse("call-1", "bash", {{"command", "make"}}));
    fixture.Script(testing::TextResponse("gave up"));
    fixture.Write("permissions.json", nlohmann::json{{"fallback", {{"execute", "ask"}}}}.dump());
    fixture.Start();

    const std::string session_id = fixture.CreateSession();
    grpc::ClientContext context;
    auto stream = fixture.stub->OpenSession(&context);
    REQUIRE(stream->Write(ChatTurn(session_id, "build it")));

    v1::ServerEvent event;
    bool tool_reported_error = false;
    while (stream->Read(&event)) {
        if (event.has_permission_request()) {
            v1::ClientEvent answer;
            answer.set_session_id(session_id);
            auto* response = answer.mutable_permission_response();
            response->set_request_id(event.permission_request().request_id());
            response->set_decision(v1::PERMISSION_DECISION_DENY);
            REQUIRE(stream->Write(answer));
        }
        if (event.has_tool_execution() && !event.tool_execution().started()) {
            tool_reported_error = event.tool_execution().is_error();
            break;
        }
    }
    (void)DrainAndFinish(stream);

    // The turn continued after the refusal rather than deadlocking, and the
    // refusal reached the model as an error result.
    CHECK(tool_reported_error);
}

TEST_CASE("a session grant silences the second prompt", "[session_service]") {
    // Consent scoping surviving SERIALISATION, which until now was only ever
    // proven in-process. Two identical tool calls: the first prompts and is
    // granted, the second must run unprompted.
    ServiceFixture fixture;
    auto tool = std::make_unique<testing::FakeTool>("bash", tools::ToolCategory::Execute, "ran it");
    tool->set_disclosure({"run make", {{"bash:/usr/bin/make", "/usr/bin/make *"}}, true});
    fixture.tools.Register(std::move(tool));
    fixture.Script(testing::ToolUseResponse("call-1", "bash", {{"command", "make"}}));
    fixture.Script(testing::ToolUseResponse("call-2", "bash", {{"command", "make"}}));
    fixture.Script(testing::TextResponse("built twice"));
    fixture.Write("permissions.json", nlohmann::json{{"fallback", {{"execute", "ask"}}}}.dump());
    fixture.Start();

    const std::string session_id = fixture.CreateSession();
    grpc::ClientContext context;
    auto stream = fixture.stub->OpenSession(&context);
    REQUIRE(stream->Write(ChatTurn(session_id, "build it twice")));

    int prompts = 0;
    int finished_tools = 0;
    v1::ServerEvent event;
    while (stream->Read(&event)) {
        if (event.has_permission_request()) {
            ++prompts;
            v1::ClientEvent answer;
            answer.set_session_id(session_id);
            auto* response = answer.mutable_permission_response();
            response->set_request_id(event.permission_request().request_id());
            response->set_decision(v1::PERMISSION_DECISION_ALLOW);
            // Take the offered grant -- "allow /usr/bin/make * this session".
            for (const auto& grant : event.permission_request().grants()) {
                response->add_granted_keys(grant.key());
            }
            REQUIRE(stream->Write(answer));
        }
        if (event.has_tool_execution() && !event.tool_execution().started()) {
            if (++finished_tools == 2) break;
        }
    }
    (void)DrainAndFinish(stream);

    CHECK(finished_tools == 2);
    CHECK(prompts == 1);  // the grant covered the second call
}

TEST_CASE("an incomplete disclosure prompts every time", "[session_service]") {
    // The vacuous-satisfaction guard, asserted across the wire: a command that
    // cannot be named must never be treated as "nothing to check, so check
    // passed", no matter how many grants are held.
    ServiceFixture fixture;
    auto tool = std::make_unique<testing::FakeTool>("bash", tools::ToolCategory::Execute, "ran it");
    tool->set_disclosure({"eval something", {{"bash:/bin/eval", "/bin/eval *"}}, false});
    fixture.tools.Register(std::move(tool));
    fixture.Script(testing::ToolUseResponse("call-1", "bash", {{"command", "eval $X"}}));
    fixture.Script(testing::ToolUseResponse("call-2", "bash", {{"command", "eval $X"}}));
    fixture.Script(testing::TextResponse("done"));
    fixture.Write("permissions.json", nlohmann::json{{"fallback", {{"execute", "ask"}}}}.dump());
    fixture.Start();

    const std::string session_id = fixture.CreateSession();
    grpc::ClientContext context;
    auto stream = fixture.stub->OpenSession(&context);
    REQUIRE(stream->Write(ChatTurn(session_id, "go")));

    int prompts = 0;
    int finished_tools = 0;
    v1::ServerEvent event;
    while (stream->Read(&event)) {
        if (event.has_permission_request()) {
            CHECK_FALSE(event.permission_request().complete());
            ++prompts;
            v1::ClientEvent answer;
            answer.set_session_id(session_id);
            auto* response = answer.mutable_permission_response();
            response->set_request_id(event.permission_request().request_id());
            response->set_decision(v1::PERMISSION_DECISION_ALLOW);
            for (const auto& grant : event.permission_request().grants()) {
                response->add_granted_keys(grant.key());
            }
            REQUIRE(stream->Write(answer));
        }
        if (event.has_tool_execution() && !event.tool_execution().started()) {
            if (++finished_tools == 2) break;
        }
    }
    (void)DrainAndFinish(stream);

    CHECK(finished_tools == 2);
    CHECK(prompts == 2);  // granted both times, and asked both times anyway
}

TEST_CASE("closing the stream terminates the session", "[session_service]") {
    // There is no detached state in v1, so a dropped stream is unambiguous.
    ServiceFixture fixture;
    fixture.Script(testing::TextResponse("hi"));
    fixture.Start();

    const std::string session_id = fixture.CreateSession();
    auto session = fixture.sessions->Find(session_id);
    REQUIRE(session != nullptr);

    {
        grpc::ClientContext context;
        auto stream = fixture.stub->OpenSession(&context);
        REQUIRE(stream->Write(ChatTurn(session_id, "hi")));
        v1::ServerEvent event;
        stream->Read(&event);
        (void)DrainAndFinish(stream);
    }

    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!session->terminated() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    CHECK(session->terminated());
    CHECK(fixture.sessions->Find(session_id) == nullptr);
}

TEST_CASE("the last client detaching signals shutdown", "[session_service]") {
    // No daemon: the server is refcounted by attached clients and goes away when
    // the last one leaves.
    ServiceFixture fixture;
    fixture.Script(testing::TextResponse("hi"));
    fixture.Start();

    const std::string session_id = fixture.CreateSession();
    {
        grpc::ClientContext context;
        auto stream = fixture.stub->OpenSession(&context);
        REQUIRE(stream->Write(ChatTurn(session_id, "hi")));
        v1::ServerEvent event;
        stream->Read(&event);
        (void)DrainAndFinish(stream);
    }

    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!fixture.last_detach.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    CHECK(fixture.last_detach.load());
    CHECK(fixture.service->attached_clients() == 0);
}

TEST_CASE("two sessions on one channel do not cross-talk", "[session_service]") {
    // One client multiplexes several sessions (tabs). Routing is by session_id,
    // and an event reaching the wrong tab would be a silent corruption.
    ServiceFixture fixture;
    fixture.Script(testing::TextResponse("answer"));
    fixture.Start();

    const std::string first = fixture.CreateSession();
    const std::string second = fixture.CreateSession();
    REQUIRE(first != second);

    grpc::ClientContext context_a;
    auto stream_a = fixture.stub->OpenSession(&context_a);
    REQUIRE(stream_a->Write(ChatTurn(first, "hi")));

    grpc::ClientContext context_b;
    auto stream_b = fixture.stub->OpenSession(&context_b);
    REQUIRE(stream_b->Write(ChatTurn(second, "hi")));

    v1::ServerEvent event;
    REQUIRE(stream_a->Read(&event));
    CHECK(event.session_id() == first);
    REQUIRE(stream_b->Read(&event));
    CHECK(event.session_id() == second);

    (void)DrainAndFinish(stream_a);
    (void)DrainAndFinish(stream_b);
}

TEST_CASE("a deferred RPC answers UNIMPLEMENTED rather than hanging", "[session_service]") {
    // A client calling one of these gets a clear status. Silence would look
    // exactly like a server that is merely slow.
    ServiceFixture fixture;
    fixture.Start();

    {
        grpc::ClientContext context;
        v1::SuggestPathsRequest request;
        v1::SuggestPathsResponse response;
        CHECK(fixture.stub->SuggestPaths(&context, request, &response).error_code() ==
              grpc::StatusCode::UNIMPLEMENTED);
    }
    {
        grpc::ClientContext context;
        v1::PluginCallRequest request;
        v1::PluginCallResponse response;
        CHECK(fixture.stub->PluginCall(&context, request, &response).error_code() ==
              grpc::StatusCode::UNIMPLEMENTED);
    }
}

TEST_CASE("an unnamed session is named from its first user message",
          "[session_service][naming]") {
    // The interaction that would otherwise ship silently. SessionDatabase only
    // sets the default name while the name is still empty -- deliberately, so an
    // explicit one is never clobbered -- so a service that stamped a placeholder
    // would suppress default naming forever and every row in the picker would
    // read the same word. Nothing below the RPC layer can catch that: the
    // storage tests call CreateSession("") directly.
    ServiceFixture fixture;
    fixture.Script(testing::TextResponse("hello there"));
    fixture.Start();

    const std::string session_id = fixture.CreateSession();  // sends no name
    grpc::ClientContext open_context;
    auto stream = fixture.stub->OpenSession(&open_context);
    REQUIRE(stream->Write(ChatTurn(session_id, "summarize the build system")));
    REQUIRE(WaitForTurnCompleted(*stream));
    (void)DrainAndFinish(stream);

    grpc::ClientContext context;
    v1::ListSessionsRequest request;
    request.set_include_all_dirs(true);
    v1::ListSessionsResponse response;
    REQUIRE(fixture.stub->ListSessions(&context, request, &response).ok());
    REQUIRE(response.sessions_size() == 1);
    CHECK(response.sessions(0).name() == "summarize the build system");
    CHECK(response.sessions(0).session_id() == session_id);
    CHECK(response.sessions(0).message_count() > 0);
    CHECK(response.sessions(0).parent_session_id().empty());
}

TEST_CASE("a turn is durable and pages back through GetMessages",
          "[session_service][scrollback]") {
    ServiceFixture fixture;
    fixture.Script(testing::TextResponse("the answer"));
    fixture.Start();

    const std::string session_id = fixture.CreateSession();
    grpc::ClientContext open_context;
    auto stream = fixture.stub->OpenSession(&open_context);
    REQUIRE(stream->Write(ChatTurn(session_id, "ask something")));
    REQUIRE(WaitForTurnCompleted(*stream));
    (void)DrainAndFinish(stream);

    grpc::ClientContext context;
    v1::GetMessagesRequest request;
    request.set_session_id(session_id);
    v1::GetMessagesResponse response;
    REQUIRE(fixture.stub->GetMessages(&context, request, &response).ok());

    // Descending by seq: the newest message first, which is how scrollback pages.
    REQUIRE(response.messages_size() == 2);
    CHECK(response.messages(1).seq() == 0);
    CHECK(response.messages(0).seq() == 1);
    CHECK(response.messages(1).message().role() == v1::ROLE_USER);
    CHECK(response.messages(0).message().role() == v1::ROLE_ASSISTANT);
    CHECK_FALSE(response.has_more());
}

TEST_CASE("scrollback for an unknown session is NOT_FOUND, not an empty page",
          "[session_service][scrollback]") {
    // An empty page and a nonexistent session are different answers: one is a
    // brand-new tab, the other is a bug the client needs to see.
    ServiceFixture fixture;
    fixture.Start();

    grpc::ClientContext context;
    v1::GetMessagesRequest request;
    request.set_session_id("01ARZ3NDEKTSV4RRFFQ69G5FAV");
    v1::GetMessagesResponse response;
    CHECK(fixture.stub->GetMessages(&context, request, &response).error_code() ==
          grpc::StatusCode::NOT_FOUND);
}

TEST_CASE("ListSessions without a project_root is refused rather than answered empty",
          "[session_service][scrollback]") {
    // Filtering on an empty project_root would match nothing and read like
    // "no sessions here" instead of like a caller that forgot a field.
    ServiceFixture fixture;
    fixture.Start();

    grpc::ClientContext context;
    v1::ListSessionsRequest request;  // neither project_root nor include_all_dirs
    v1::ListSessionsResponse response;
    CHECK(fixture.stub->ListSessions(&context, request, &response).error_code() ==
          grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_CASE("a failed CreateSession leaves no session row behind",
          "[session_service][naming]") {
    // The database mints the id, so its row exists before the Session does. If
    // constructing the Session then fails, that row has to go -- otherwise every
    // preset typo leaves a message-less session in the picker permanently.
    ServiceFixture fixture;
    fixture.Start();

    {
        grpc::ClientContext context;
        v1::CreateSessionRequest request;
        request.set_agent_preset("nonexistent");
        request.set_project_root("/tmp");
        v1::CreateSessionResponse response;
        CHECK(fixture.stub->CreateSession(&context, request, &response).error_code() ==
              grpc::StatusCode::FAILED_PRECONDITION);
    }

    grpc::ClientContext context;
    v1::ListSessionsRequest request;
    request.set_include_all_dirs(true);
    v1::ListSessionsResponse response;
    REQUIRE(fixture.stub->ListSessions(&context, request, &response).ok());
    CHECK(response.sessions_size() == 0);
}

TEST_CASE("a command invocation is answered while the subsystem is missing",
          "[session_service]") {
    ServiceFixture fixture;
    fixture.Script(testing::TextResponse("hi"));
    fixture.Start();

    const std::string session_id = fixture.CreateSession();
    grpc::ClientContext context;
    auto stream = fixture.stub->OpenSession(&context);

    v1::ClientEvent invocation;
    invocation.set_session_id(session_id);
    invocation.mutable_command()->set_request_id("cmd-1");
    invocation.mutable_command()->set_name("compact");
    REQUIRE(stream->Write(invocation));

    v1::ServerEvent event;
    bool answered = false;
    while (stream->Read(&event)) {
        if (event.has_command_result()) {
            CHECK(event.command_result().request_id() == "cmd-1");
            CHECK(event.command_result().status() == v1::COMMAND_STATUS_ERROR);
            answered = true;
            break;
        }
    }
    (void)DrainAndFinish(stream);
    CHECK(answered);
}

TEST_CASE("a second client on the same session is refused, not silently served",
          "[session_service][invariant]") {
    // The outbound queue has ONE consumer by design (ARCHITECTURE.md,
    // "Concurrency model"): events buffer on the SESSION so a producer never has
    // to know whether anyone is listening. Two streams draining it would give
    // each a random half of the events, and the first to close would terminate
    // the session under the other -- which looks like flaky output, not a
    // protocol error. MarkAttached was a latch that never said no.
    ServiceFixture fixture;
    fixture.Script(testing::TextResponse("hi"));
    fixture.Start();

    const std::string session_id = fixture.CreateSession();

    grpc::ClientContext first_context;
    auto first = fixture.stub->OpenSession(&first_context);
    v1::ClientEvent naming;
    naming.set_session_id(session_id);
    naming.mutable_chat_turn()->set_text("hello");
    REQUIRE(first->Write(naming));

    // Wait for the first stream to actually be attached; OpenSession claims the
    // session on its first inbound message, not at connect.
    v1::ServerEvent event;
    REQUIRE(first->Read(&event));

    grpc::ClientContext second_context;
    auto second = fixture.stub->OpenSession(&second_context);
    REQUIRE(second->Write(naming));
    v1::ServerEvent unused;
    while (second->Read(&unused)) {
    }
    const auto status = second->Finish();
    CHECK(status.error_code() == grpc::StatusCode::ALREADY_EXISTS);

    // The first stream is unharmed: it still owns the session and its turn.
    (void)DrainAndFinish(first);
}
