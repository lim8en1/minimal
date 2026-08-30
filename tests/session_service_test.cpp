// End-to-end over a real gRPC channel on a unix socket -- the fourth test tier
#include <algorithm>
// in ARCHITECTURE.md, and the first tier this project has ever had. Everything
// below the provider is real: real service, real Session, real worker thread,
// real ToolExecutor, real permission round-trip.

#include <sys/stat.h>
#include <unistd.h>

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
#include "temp_dir.hpp"
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
        Write("permissions.json", "{}");
        Write("agents/main.md", "---\nname: main\nmodel: main/test-model\nallowed_tools: [\"*\"]\n---\nYou help.\n");

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
        providers.Register("fake", [script_copy, this](const llm::ProviderConfig&) {
            auto provider = std::make_unique<testing::FakeLlmProvider>();
            for (const auto& step : script_copy->steps) {
                provider->PushResponse(step.response, step.events);
            }
            // Recorded so tests can assert what a session's provider RECEIVED
            // (e.g. that a resumed session replayed its stored history). Valid
            // only while that session is alive -- the Session owns its provider,
            // so assert before the session's stream closes.
            created_providers.push_back(provider.get());
            return provider;
        });
        observers = std::make_unique<agent::SessionObserverRegistry>(common::DefaultLogger());
        db = std::make_unique<session::SessionDatabase>(dir / "sessions.db");
        sessions_commands = &commands;
        sessions = std::make_unique<agent::SessionRegistry>(
            *presets, tools, providers, *observers, *db, agent::LoopConfig{},
            common::DefaultLogger(), attach_guard);
        service = std::make_unique<rpc::SessionServiceImpl>(
            *sessions, *db, config::SettingsConfig{}, config::PermissionsConfig{},
            common::DefaultLogger());
        sessions->set_commands(&commands);
        service->set_commands(&commands);
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

    commands::CommandRegistry commands;
    const commands::CommandRegistry* sessions_commands = nullptr;
    testing::TempDir tempdir;
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
    std::vector<testing::FakeLlmProvider*> created_providers;
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

TEST_CASE("CreateSession refuses a project_root that is not an existing directory",
          "[session_service]") {
    // project_root is the confinement boundary; validating it here means a bad
    // client-supplied path fails fast with a reason instead of surfacing later
    // as confusing tool errors.
    ServiceFixture fixture;
    fixture.Start();

    // A path that does not exist.
    {
        grpc::ClientContext context;
        v1::CreateSessionRequest request;
        request.set_agent_preset("main");
        request.set_project_root((fixture.dir / "no-such-dir").string());
        v1::CreateSessionResponse response;
        const auto status = fixture.stub->CreateSession(&context, request, &response);
        CHECK(status.error_code() == grpc::StatusCode::FAILED_PRECONDITION);
        CHECK(status.error_message().find("not an existing directory") != std::string::npos);
    }
    // A path that exists but is a file, not a directory.
    {
        const auto file = fixture.dir / "work" / "a-file.txt";
        { std::ofstream(file) << "x"; }
        grpc::ClientContext context;
        v1::CreateSessionRequest request;
        request.set_agent_preset("main");
        request.set_project_root(file.string());
        v1::CreateSessionResponse response;
        const auto status = fixture.stub->CreateSession(&context, request, &response);
        CHECK(status.error_code() == grpc::StatusCode::FAILED_PRECONDITION);
    }
}

TEST_CASE("CreateSession refuses a project_root it cannot write", "[session_service]") {
    // POSIX access(2): this libc++ ships no std::filesystem writability probe.
    // Skipped when the check is meaningless (e.g. root bypasses mode bits).
    ServiceFixture fixture;
    fixture.Start();

    const auto ro = fixture.dir / "read-only-work";
    std::filesystem::create_directories(ro);
    REQUIRE(::chmod(ro.c_str(), 0555) == 0);
    if (::access(ro.c_str(), W_OK) == 0) {  // likely root: mode bits do not apply
        ::chmod(ro.c_str(), 0755);
        return;
    }

    grpc::ClientContext context;
    v1::CreateSessionRequest request;
    request.set_agent_preset("main");
    request.set_project_root(ro.string());
    v1::CreateSessionResponse response;
    const auto status = fixture.stub->CreateSession(&context, request, &response);
    ::chmod(ro.c_str(), 0755);  // restore before fixture teardown

    CHECK(status.error_code() == grpc::StatusCode::FAILED_PRECONDITION);
    CHECK(status.error_message().find("not writable") != std::string::npos);
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

TEST_CASE("CancelTurn over the wire ends the turn and leaves the session usable",
          "[session_service][invariant]") {
    // The reader thread's kCancel branch had never executed under test.
    // Cancellation was covered in-process only, so the one path that matters to a
    // real client -- a CancelTurn ClientEvent arriving mid-turn on the bidi
    // stream -- was assumed rather than exercised. The post-merge review then
    // changed exactly this path (Session::Cancel now bumps a generation instead
    // of latching a flag), which is the worst possible combination.
    //
    // A permission prompt is the rendezvous: it parks the worker at a known
    // point, so "mid-turn" is deterministic rather than a sleep.
    ServiceFixture fixture;
    fixture.tools.Register(
        std::make_unique<testing::FakeTool>("bash", tools::ToolCategory::Execute, "ran it"));
    // Turn 1 makes exactly ONE provider call: the cancel breaks the loop right
    // after the denied tool, before it would ask again. Scripting a second
    // response for it would silently hand that response to turn 2 instead.
    fixture.Script(testing::ToolUseResponse("call-1", "bash", {{"command", "make"}}));
    fixture.Script(testing::ToolUseResponse("call-2", "bash", {{"command", "make"}}));
    fixture.Script(testing::TextResponse("second turn finished"));
    fixture.Write("permissions.json", nlohmann::json{{"fallback", {{"execute", "ask"}}}}.dump());
    fixture.Start();

    const std::string session_id = fixture.CreateSession();
    grpc::ClientContext context;
    auto stream = fixture.stub->OpenSession(&context);
    REQUIRE(stream->Write(ChatTurn(session_id, "build it")));

    // Turn 1: cancel instead of answering the prompt.
    v1::ServerEvent event;
    bool cancelled_turn_completed = false;
    while (stream->Read(&event)) {
        if (event.has_permission_request()) {
            v1::ClientEvent cancel;
            cancel.set_session_id(session_id);
            cancel.mutable_cancel();
            REQUIRE(stream->Write(cancel));
        }
        if (event.has_turn_completed()) {
            cancelled_turn_completed = true;
            break;
        }
    }
    // Emitted on every exit path, cancellation included -- a client that learns a
    // turn ended only by seeing it succeed hangs here.
    CHECK(cancelled_turn_completed);

    // Turn 2: the session is still alive AND still able to ask. A cancel that
    // poisoned the rendezvous would auto-deny here, silently, forever.
    REQUIRE(stream->Write(ChatTurn(session_id, "try again")));
    bool prompted_again = false;
    bool tool_ran = false;
    while (stream->Read(&event)) {
        if (event.has_permission_request()) {
            prompted_again = true;
            v1::ClientEvent answer;
            answer.set_session_id(session_id);
            auto* response = answer.mutable_permission_response();
            response->set_request_id(event.permission_request().request_id());
            response->set_decision(v1::PERMISSION_DECISION_ALLOW);
            REQUIRE(stream->Write(answer));
        }
        if (event.has_tool_execution() && !event.tool_execution().started()) {
            tool_ran = !event.tool_execution().is_error();
        }
        if (event.has_turn_completed()) break;
    }
    (void)DrainAndFinish(stream);
    CHECK(prompted_again);
    CHECK(tool_ran);
}

TEST_CASE("a turn's ServerEvent payloads arrive in order", "[session_service]") {
    // The round-trip test checks that events arrive and carry the right
    // session_id; it never checked the ORDER, which is the part a client's state
    // machine actually depends on. TurnCompletedEvent arriving before the deltas
    // it summarises would break any client that treats it as the terminator --
    // which is the entire reason it exists.
    ServiceFixture fixture;
    fixture.tools.Register(
        std::make_unique<testing::FakeTool>("read", tools::ToolCategory::ReadOnly, "contents"));
    fixture.Script(testing::ToolUseResponse("call-1", "read", {{"path", "a.txt"}}));
    fixture.Script(testing::TextResponse("done"));
    fixture.Start();

    const std::string session_id = fixture.CreateSession();
    grpc::ClientContext context;
    auto stream = fixture.stub->OpenSession(&context);
    REQUIRE(stream->Write(ChatTurn(session_id, "read it")));

    std::vector<std::string> order;
    v1::ServerEvent event;
    while (stream->Read(&event)) {
        if (event.has_stream_event()) {
            order.push_back(event.stream_event().has_message_stop() ? "message_stop" : "stream");
        } else if (event.has_tool_execution()) {
            order.push_back(event.tool_execution().started() ? "tool_start" : "tool_finish");
        } else if (event.has_turn_completed()) {
            order.push_back("turn_completed");
            break;
        }
    }
    (void)DrainAndFinish(stream);

    REQUIRE_FALSE(order.empty());
    // The terminator is last, and appears once. Everything else is what it
    // summarises, so anything after it would be unreachable to a conforming
    // client -- it stops reading here.
    CHECK(order.back() == "turn_completed");
    CHECK(std::count(order.begin(), order.end(), "turn_completed") == 1);

    // A tool is bracketed: started before finished, and both before the turn ends.
    const auto start = std::find(order.begin(), order.end(), "tool_start");
    const auto finish = std::find(order.begin(), order.end(), "tool_finish");
    REQUIRE(start != order.end());
    REQUIRE(finish != order.end());
    CHECK(start < finish);

    // The tool ran between two provider calls, so a message_stop precedes it and
    // another follows -- the shape of a turn with one tool round.
    const auto first_stop = std::find(order.begin(), order.end(), "message_stop");
    REQUIRE(first_stop != order.end());
    CHECK(first_stop < start);
    CHECK(std::count(order.begin(), order.end(), "message_stop") == 2);
}

TEST_CASE("a read-only command answers while a turn is in flight",
          "[session_service][commands][invariant]") {
    // THE DISPATCH SPLIT, and the only tier that can show it. A read-only
    // command runs on a short-lived task rather than on the reader thread --
    // running it inline would block the very stream its answer has to travel
    // back down, which is the deadlock the three-thread topology exists to
    // prevent, and queueing it would make it wait for the turn it is supposed to
    // run beside.
    ServiceFixture fixture;
    fixture.commands.Register(std::make_unique<testing::BlockingReadOnlyCommand>());
    fixture.tools.Register(
        std::make_unique<testing::FakeTool>("bash", tools::ToolCategory::Execute, "ran"));
    fixture.Script(testing::ToolUseResponse("call-1", "bash", {{"command", "make"}}));
    fixture.Script(testing::TextResponse("done"));
    fixture.Write("permissions.json", nlohmann::json{{"fallback", {{"execute", "ask"}}}}.dump());
    fixture.Start();

    const std::string session_id = fixture.CreateSession();
    grpc::ClientContext context;
    auto stream = fixture.stub->OpenSession(&context);
    REQUIRE(stream->Write(ChatTurn(session_id, "build it")));

    // Park the turn on a permission prompt, so "in flight" is a fact rather
    // than a race.
    v1::ServerEvent event;
    std::string permission_id;
    while (stream->Read(&event)) {
        if (event.has_permission_request()) {
            permission_id = event.permission_request().request_id();
            break;
        }
    }
    REQUIRE_FALSE(permission_id.empty());

    // The turn is blocked. A read-only command must still be answered.
    v1::ClientEvent invocation;
    invocation.set_session_id(session_id);
    invocation.mutable_command()->set_request_id("cmd-1");
    invocation.mutable_command()->set_name("peek");
    REQUIRE(stream->Write(invocation));

    bool answered = false;
    while (stream->Read(&event)) {
        if (event.has_command_result() && event.command_result().request_id() == "cmd-1") {
            CHECK(event.command_result().status() == v1::COMMAND_STATUS_OK);
            answered = true;
            break;
        }
    }
    CHECK(answered);

    // Release the turn so the fixture tears down cleanly.
    v1::ClientEvent answer;
    answer.set_session_id(session_id);
    answer.mutable_permission_response()->set_request_id(permission_id);
    answer.mutable_permission_response()->set_decision(v1::PERMISSION_DECISION_DENY);
    stream->Write(answer);
    (void)DrainAndFinish(stream);
}

TEST_CASE("a mutating command lands at a turn boundary, never mid-tool-cycle",
          "[session_service][commands][invariant]") {
    // Structural rather than checked: the worker pops one work item at a time
    // and RunTurn is atomic from its point of view, so a command can only land
    // between turns. What this asserts is that the queue is actually shared --
    // dispatching a mutating command straight to a task instead would let it
    // run between a tool_use and its tool_result, which is a hard 400.
    ServiceFixture fixture;
    fixture.commands.Register(std::make_unique<testing::RecordingCommand>());
    fixture.tools.Register(
        std::make_unique<testing::FakeTool>("read", tools::ToolCategory::ReadOnly, "contents"));
    fixture.Script(testing::ToolUseResponse("call-1", "read", {{"path", "a.txt"}}));
    fixture.Script(testing::TextResponse("done"));
    fixture.Start();

    const std::string session_id = fixture.CreateSession();
    grpc::ClientContext context;
    auto stream = fixture.stub->OpenSession(&context);

    // Both sent before anything is read, so the command is queued while the turn
    // is still running.
    REQUIRE(stream->Write(ChatTurn(session_id, "read it")));
    v1::ClientEvent invocation;
    invocation.set_session_id(session_id);
    invocation.mutable_command()->set_request_id("cmd-1");
    invocation.mutable_command()->set_name("record");
    REQUIRE(stream->Write(invocation));

    std::vector<std::string> order;
    v1::ServerEvent event;
    while (stream->Read(&event)) {
        if (event.has_tool_execution()) {
            order.push_back(event.tool_execution().started() ? "tool_start" : "tool_finish");
        } else if (event.has_turn_completed()) {
            order.push_back("turn_completed");
        } else if (event.has_command_result()) {
            order.push_back("command");
            break;
        }
    }
    (void)DrainAndFinish(stream);

    // The command ran AFTER the turn ended -- not between the tool's start and
    // its finish, which is where an immediate dispatch would have put it.
    REQUIRE(order.size() >= 2);
    CHECK(order.back() == "command");
    const auto finish = std::find(order.begin(), order.end(), "tool_finish");
    const auto completed = std::find(order.begin(), order.end(), "turn_completed");
    REQUIRE(finish != order.end());
    REQUIRE(completed != order.end());
    CHECK(finish < completed);
}

// --- Resume -------------------------------------------------------------------

namespace {

// Waits for a session to leave the registry, which happens when its stream
// closes and the termination completes.
void WaitForTerminated(agent::SessionRegistry& sessions, const std::string& session_id) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (sessions.Find(session_id) != nullptr && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    REQUIRE(sessions.Find(session_id) == nullptr);
}

grpc::Status ResumeOverWire(v1::SessionService::Stub& stub, const std::string& session_id,
                            v1::CreateSessionResponse* response, const std::string& model = {}) {
    grpc::ClientContext context;
    v1::CreateSessionRequest request;
    request.set_session_id(session_id);
    request.set_model(model);
    return stub.CreateSession(&context, request, response);
}

}  // namespace

TEST_CASE("a stored session resumes with its history intact", "[session_service][resume]") {
    // The gap this closes: a transcript could be READ (GetMessages, ListSessions)
    // but not resumed -- the loop had no seeded-history path, so a reopened
    // session would either start blank or re-append the whole transcript as
    // duplicate rows.
    ServiceFixture fixture;
    fixture.Script(testing::TextResponse("first answer"));
    fixture.Start();

    // Phase 1: an ordinary session, one turn, then the stream closes and it
    // terminates -- the transcript survives in SQLite.
    const std::string session_id = fixture.CreateSession();
    {
        grpc::ClientContext context;
        auto stream = fixture.stub->OpenSession(&context);
        REQUIRE(stream->Write(ChatTurn(session_id, "remember the number 7")));
        REQUIRE(WaitForTurnCompleted(*stream));
        (void)DrainAndFinish(stream);
    }
    WaitForTerminated(*fixture.sessions, session_id);

    // Phase 2: resume. The resumed incarnation gets its own script.
    fixture.script->steps.clear();
    fixture.Script(testing::TextResponse("it was 7"));

    v1::CreateSessionResponse resumed;
    REQUIRE(ResumeOverWire(*fixture.stub, session_id, &resumed).ok());
    CHECK(resumed.session().session_id() == session_id);  // the SAME session
    CHECK(resumed.session().message_count() == 2);        // its stored transcript

    grpc::ClientContext context;
    auto stream = fixture.stub->OpenSession(&context);
    REQUIRE(stream->Write(ChatTurn(session_id, "what number did I give you?")));
    REQUIRE(WaitForTurnCompleted(*stream));

    // The resumed session's provider received the stored history ahead of the
    // new turn -- asserted while the session is still alive (the Session owns
    // its provider). Removing the seeded-history load fails this: the provider
    // would see only the new user message.
    REQUIRE(fixture.created_providers.size() == 2);
    const auto* provider = fixture.created_providers.back();
    REQUIRE(provider->requests().size() == 1);
    const auto& sent = provider->requests().front().messages;
    REQUIRE(sent.size() == 3);
    CHECK(std::get<llm::TextBlock>(sent[0].content.front()).text == "remember the number 7");
    CHECK(std::get<llm::TextBlock>(sent[1].content.front()).text == "first answer");
    CHECK(std::get<llm::TextBlock>(sent[2].content.front()).text == "what number did I give you?");

    (void)DrainAndFinish(stream);
    WaitForTerminated(*fixture.sessions, session_id);

    // The transcript is one contiguous sequence: stored rows kept, new rows
    // continuing at MAX(seq) + 1, and NOTHING duplicated. Re-appending the
    // seed would show up here as repeated content.
    grpc::ClientContext messages_context;
    v1::GetMessagesRequest messages_request;
    messages_request.set_session_id(session_id);
    v1::GetMessagesResponse messages;
    REQUIRE(fixture.stub->GetMessages(&messages_context, messages_request, &messages).ok());
    REQUIRE(messages.messages_size() == 4);
    for (int i = 0; i < 4; ++i) {
        CHECK(messages.messages(i).seq() == 3 - i);  // descending on the wire
    }
    CHECK(messages.messages(3).message().content(0).text().text() == "remember the number 7");
    CHECK(messages.messages(2).message().content(0).text().text() == "first answer");
    CHECK(messages.messages(1).message().content(0).text().text() == "what number did I give you?");
    CHECK(messages.messages(0).message().content(0).text().text() == "it was 7");

    // Still exactly one session -- resume never mints a row.
    grpc::ClientContext list_context;
    v1::ListSessionsRequest list_request;
    list_request.set_include_all_dirs(true);
    v1::ListSessionsResponse list;
    REQUIRE(fixture.stub->ListSessions(&list_context, list_request, &list).ok());
    REQUIRE(list.sessions_size() == 1);
    CHECK(list.sessions(0).session_id() == session_id);
    CHECK(list.sessions(0).message_count() == 4);
}

TEST_CASE("resume validates its request", "[session_service][resume]") {
    ServiceFixture fixture;
    fixture.Script(testing::TextResponse("hi"));
    fixture.Start();

    SECTION("an unknown session is NOT_FOUND") {
        v1::CreateSessionResponse response;
        const auto status = ResumeOverWire(*fixture.stub, "01ARZ3NDEKTSV4RRFFQ69G5FAV", &response);
        CHECK(status.error_code() == grpc::StatusCode::NOT_FOUND);
    }

    SECTION("creation fields are refused rather than silently ignored") {
        const std::string session_id = fixture.CreateSession();
        grpc::ClientContext context;
        v1::CreateSessionRequest request;
        request.set_session_id(session_id);
        request.set_agent_preset("main");  // the stored preset governs
        v1::CreateSessionResponse response;
        const auto status = fixture.stub->CreateSession(&context, request, &response);
        CHECK(status.error_code() == grpc::StatusCode::INVALID_ARGUMENT);
        // The refused resume left the original session untouched.
        CHECK(fixture.sessions->Find(session_id) != nullptr);
    }

    SECTION("an already-open session is refused") {
        const std::string session_id = fixture.CreateSession();
        grpc::ClientContext open_context;
        auto stream = fixture.stub->OpenSession(&open_context);
        REQUIRE(stream->Write(ChatTurn(session_id, "stay open")));
        v1::ServerEvent event;
        REQUIRE(stream->Read(&event));  // attached now

        v1::CreateSessionResponse response;
        const auto status = ResumeOverWire(*fixture.stub, session_id, &response);
        CHECK(status.error_code() == grpc::StatusCode::ALREADY_EXISTS);

        (void)DrainAndFinish(stream);
    }

    SECTION("a subagent session cannot be resumed") {
        const auto parent = fixture.db->CreateSession("parent", std::nullopt, "/p", "main");
        const auto child = fixture.db->CreateSession("child", parent.session_id, "/p", "main");
        v1::CreateSessionResponse response;
        const auto status = ResumeOverWire(*fixture.stub, child.session_id, &response);
        CHECK(status.error_code() == grpc::StatusCode::FAILED_PRECONDITION);
    }
}

TEST_CASE("resume repairs a transcript torn mid-tool-sequence", "[session_service][resume]") {
    // The torn state is what a CRASH leaves: the assistant's tool_use persisted,
    // its results never written. A live cancellation cannot produce it -- a
    // cancelled turn still appends denial results before breaking -- so the rows
    // are crafted directly, exactly as a killed server process would leave them.
    // Replaying the tail as-is is a hard 400 on both wire formats, so resume
    // must persist synthesized error results BEFORE the session runs.
    ServiceFixture fixture;
    fixture.tools.Register(
        std::make_unique<testing::FakeTool>("bash", tools::ToolCategory::Execute, "ran it"));
    fixture.Script(testing::TextResponse("recovered"));
    fixture.Start();

    const auto row = fixture.db->CreateSession("", std::nullopt,
                                               (fixture.dir / "work").string(), "main");
    fixture.db->AppendMessage(row.session_id,
                              llm::Message{llm::Role::User, {llm::TextBlock{"build it"}}});
    llm::Message tool_use{llm::Role::Assistant, {}};
    tool_use.content.push_back(llm::ToolUseBlock{"call-1", "bash", {{"command", "make"}}});
    fixture.db->AppendMessage(row.session_id, tool_use);
    // ...and nothing after it: the process died before results were appended.

    v1::CreateSessionResponse resumed;
    REQUIRE(ResumeOverWire(*fixture.stub, row.session_id, &resumed).ok());

    // The repair is a REAL persisted row: user-role, one error ToolResultBlock
    // per unanswered tool_use, sitting between the torn tail and anything the
    // resumed session appends.
    grpc::ClientContext messages_context;
    v1::GetMessagesRequest messages_request;
    messages_request.set_session_id(row.session_id);
    v1::GetMessagesResponse messages;
    REQUIRE(fixture.stub->GetMessages(&messages_context, messages_request, &messages).ok());
    REQUIRE(messages.messages_size() == 3);
    CHECK(messages.messages(2).seq() == 0);  // user "build it"
    CHECK(messages.messages(1).seq() == 1);  // assistant tool_use
    REQUIRE(messages.messages(0).seq() == 2);
    REQUIRE(messages.messages(0).message().role() == v1::ROLE_USER);
    REQUIRE(messages.messages(0).message().content_size() == 1);
    REQUIRE(messages.messages(0).message().content(0).has_tool_result());
    CHECK(messages.messages(0).message().content(0).tool_result().tool_use_id() == "call-1");
    CHECK(messages.messages(0).message().content(0).tool_result().is_error());

    // And the resumed session actually runs on top of the repair.
    grpc::ClientContext context;
    auto stream = fixture.stub->OpenSession(&context);
    REQUIRE(stream->Write(ChatTurn(row.session_id, "carry on")));
    REQUIRE(WaitForTurnCompleted(*stream));

    // Its first provider call saw the repaired history -- a dangling tool_use
    // reaching the provider would mean the repair never happened.
    const auto* provider = fixture.created_providers.back();
    REQUIRE(provider->requests().size() == 1);
    const auto& sent = provider->requests().front().messages;
    REQUIRE(sent.size() == 4);
    REQUIRE(sent[2].content.size() == 1);
    const auto* repair = std::get_if<llm::ToolResultBlock>(&sent[2].content.front());
    REQUIRE(repair != nullptr);
    CHECK(repair->tool_use_id == "call-1");
    CHECK(repair->is_error);

    (void)DrainAndFinish(stream);
    WaitForTerminated(*fixture.sessions, row.session_id);
}

TEST_CASE("a failed resume leaves the stored session intact", "[session_service][resume]") {
    // Unlike fresh creation -- which rolls back its just-minted row when the
    // Session fails to construct -- a failed resume must never touch the row:
    // it is the user's existing session, not one this call made.
    ServiceFixture fixture;
    fixture.Script(testing::TextResponse("hi"));
    fixture.Start();

    const std::string session_id = fixture.CreateSession();

    // A resume that fails validation never reaches the registry.
    grpc::ClientContext context;
    v1::CreateSessionRequest request;
    request.set_session_id(session_id);
    request.set_name("something else");
    v1::CreateSessionResponse response;
    CHECK(fixture.stub->CreateSession(&context, request, &response).error_code() ==
          grpc::StatusCode::INVALID_ARGUMENT);

    grpc::ClientContext list_context;
    v1::ListSessionsRequest list_request;
    list_request.set_include_all_dirs(true);
    v1::ListSessionsResponse list;
    REQUIRE(fixture.stub->ListSessions(&list_context, list_request, &list).ok());
    REQUIRE(list.sessions_size() == 1);
    CHECK(list.sessions(0).session_id() == session_id);
}

TEST_CASE("the model flag applies on resume, and a malformed one is refused there too",
          "[session_service][resume][invariant]") {
    // CROSS-FEATURE: resume builds its own SessionParams, so it silently dropped
    // the model override -- the client sent it, the server accepted the request,
    // and nothing happened. That is precisely the failure resume's own field
    // checks refuse to make for name/agent_preset/project_root.
    //
    // It APPLIES rather than being rejected because a model the user names is a
    // property of the invocation, like run_mode, and the override is in-memory
    // only -- a resumed session has none of its own to preserve.
    ServiceFixture fixture;
    fixture.Write("providers.json",
                  nlohmann::json{{"providers",
                                  {{"main",
                                    {{"backend", "fake"},
                                     {"url", "http://localhost/v1"},
                                     {"models",
                                      {{"test-model", nlohmann::json::object()},
                                       {"other-model", {{"max_tokens", 512}}}}}}}}}}
                      .dump());
    fixture.Script(testing::TextResponse("first"));
    fixture.Start();

    const std::string session_id = fixture.CreateSession();
    {
        grpc::ClientContext context;
        auto stream = fixture.stub->OpenSession(&context);
        REQUIRE(stream->Write(ChatTurn(session_id, "hello")));
        REQUIRE(WaitForTurnCompleted(*stream));
        (void)DrainAndFinish(stream);
    }
    WaitForTerminated(*fixture.sessions, session_id);

    SECTION("a well-formed override takes effect on the resumed session") {
        fixture.script->steps.clear();
        fixture.Script(testing::TextResponse("resumed"));

        v1::CreateSessionResponse resumed;
        REQUIRE(ResumeOverWire(*fixture.stub, session_id, &resumed, "main/other-model").ok());

        auto session = fixture.sessions->Find(session_id);
        REQUIRE(session != nullptr);
        CHECK(session->preset().model_ref.str() == "main/other-model");
        REQUIRE(session->preset().max_tokens.has_value());
        CHECK(*session->preset().max_tokens == 512);
    }

    SECTION("a malformed override is INVALID_ARGUMENT, not a silent no-op") {
        v1::CreateSessionResponse resumed;
        const auto status = ResumeOverWire(*fixture.stub, session_id, &resumed, "no-slash");
        CHECK(status.error_code() == grpc::StatusCode::INVALID_ARGUMENT);
        CHECK(fixture.sessions->Find(session_id) == nullptr);  // not opened
    }
}
