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
#include <limits>
#include <optional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include <catch2/catch_test_macros.hpp>
#include <grpcpp/create_channel.h>

#include "deadline.hpp"
#include "fakes.hpp"
#include "temp_dir.hpp"
#include "../../plugins/server/commands-core/core_commands.hpp"
#include "minimal/rpc/session_wiring.hpp"
#include "minimal/agent/signal_router.hpp"
#include "minimal/client/session_client.hpp"
#include "minimal/plugins/plugin_loader.hpp"
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
struct ServiceFixture {
    ServiceFixture() {
        // A unique arena per process: catch_discover_tests runs every TEST_CASE
        // as its own ctest entry, so a fixed path would be wiped out from under
        // a concurrent run of this very file (ctest -j).
        // TempDir, not a temp_directory_path()/<long-name> arena: the unix
        // socket lives in here, and macOS caps sun_path at ~104 bytes -- a
        // readable-but-long arena name silently fails to bind.
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
        Write("settings.json", nlohmann::json{{"default_agent_preset", "main"},
                                              {"default_model", "main/test-model"}}.dump());
        Write("permissions.json", "{}");
        Write("agents/main.md", "---\nname: main\nmodel: main/test-model\nallowed_tools: [\"*\"]\n---\nYou help.\n");

        // ONE SHARED SCRIPT, consumed in real time -- the same multi-session
        // determinism ScriptedProviderScript was built for. Provider instances
        // are SHARED across sessions by ProviderInstanceCache (they are
        // stateless by contract), so a fixture that handed each session its own
        // seeded copy would fight the design: resume and multi-session tests
        // would get a provider whose queue was already consumed. Pushing onto
        // the live script works for every session, whenever it is pushed.
        script = std::make_shared<testing::ScriptedProviderScript>();
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

    // Loads the advisor fixture through the REAL loader -- the first
    // service-level test to run PluginLoader, because a registration seam
    // proven only in-process proves nothing about symbol lookup or attribution.
    void LoadAdvisorPlugin() {
        plugins::PluginCandidate candidate;
        candidate.file = std::filesystem::path(MINIMAL_FIXTURE_DIR) / "fixture-advisor.so";
        candidate.provenance = plugins::Provenance::User;
        candidate.enabled = true;
        plugins::PluginLoader loader(plugins::HostRegistries{
            .tools = &tools, .providers = &providers, .permission_advisors = &permission_advisors});
        const auto records = loader.LoadAll({candidate});
        REQUIRE(records.size() == 1);
        REQUIRE(records.front().ok());
        REQUIRE(!records.front().Registered("advisor").empty());
    }

    // Same, with a config slice for plugins whose registration reads one
    // (the typesafe-advisor E2E: the loopback stub URL rides the plugin's own
    // "config" object, exactly as a real plugins.json entry would carry it).
    void LoadAdvisorPlugin(const nlohmann::json& config, const std::string& file) {
        plugins::PluginCandidate candidate;
        candidate.file = std::filesystem::path(MINIMAL_FIXTURE_DIR) / file;
        candidate.provenance = plugins::Provenance::User;
        candidate.enabled = true;
        candidate.config = config;
        plugins::PluginLoader loader(plugins::HostRegistries{
            .tools = &tools, .providers = &providers, .permission_advisors = &permission_advisors});
        const auto records = loader.LoadAll({candidate});
        REQUIRE(records.size() == 1);
        REQUIRE(records.front().ok());
        REQUIRE(!records.front().Registered("advisor").empty());
    }

    // Loads the generic-channel endpoint fixture through the REAL loader, so
    // PluginCall/PluginSubscribe dispatch runs against a dlopen'd PluginEndpoint.
    void LoadEndpointPlugin() {
        plugins::PluginCandidate candidate;
        candidate.file = std::filesystem::path(MINIMAL_FIXTURE_DIR) / "fixture-endpoint.so";
        candidate.provenance = plugins::Provenance::User;
        candidate.enabled = true;
        // The endpoint lands under the plugin's own name, which is what the
        // RPC dispatches by.
        plugins::PluginLoader loader(
            plugins::HostRegistries{.tools = &tools, .providers = &providers, .services = &services});
        const auto records = loader.LoadAll({candidate});
        REQUIRE(records.size() == 1);
        REQUIRE(records.front().ok());
        REQUIRE(records.front().Registered("service").size() == 1);
    }

    // Built after the script and any tools, so a session sees the finished setup.
    void Start() {
        catalog = std::make_unique<config::ProvidersConfig>(
            config::LoadProvidersConfig(dir / "providers.json"));
        presets = std::make_unique<agent::AgentPresetRegistry>(
            dir, config::LoadSettingsConfig(dir / "settings.json"),
            config::LoadPermissionsConfig(dir / "permissions.json"), *catalog);
        auto script_copy = script;
        providers.Register("fake", [script_copy](const llm::ProviderConfig&) {
            return std::make_unique<testing::ScriptedProvider>(script_copy);
        });
        observers = std::make_unique<agent::SessionObserverRegistry>(common::DefaultLogger());
        db = std::make_unique<session::SessionDatabase>(dir / "sessions.db");
        sessions_commands = &commands;
        const plugins::HostRegistries registries{.tools = &tools,
                                                 .providers = &providers,
                                                 .commands = &commands,
                                                 .observers = observers.get(),
                                                 .permission_advisors = &permission_advisors,
                                                 .mention_resolvers = &mention_resolvers,
                                                 .services = &services};
        sessions = std::make_unique<agent::SessionRegistry>(
            *presets, rpc::FinishSessionWiring, registries, *db, agent::LoopConfig{}, common::DefaultLogger(), attach_guard);
        rpc::SessionServiceImpl::Options service_options;
        service_options.presets = presets.get();
        service_options.signal_router = &signal_router;
        service = std::make_unique<rpc::SessionServiceImpl>(
            *sessions, *db, config::LoadSettingsConfig(dir / "settings.json"), service_permissions,
            common::DefaultLogger(), registries, std::move(service_options));
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

    std::string CreateSession() { return CreateSessionWithSignals({}); }

    std::string CreateSessionWithSignals(const std::vector<std::string>& selectors) {
        grpc::ClientContext context;
        v1::CreateSessionRequest request;
        request.set_project_root((dir / "work").string());
        request.set_agent_preset("main");
        for (const auto& selector : selectors) request.add_signals(selector);
        v1::CreateSessionResponse response;
        const auto status = stub->CreateSession(&context, request, &response);
        REQUIRE(status.ok());
        return response.session().session_id();
    }

    commands::CommandRegistry commands;
    const commands::CommandRegistry* sessions_commands = nullptr;
    agent::MentionResolverRegistry mention_resolvers{common::DefaultLogger()};
    plugins::ServiceRegistry services;
    testing::TempDir tempdir;
    std::filesystem::path dir, socket;
    tools::ToolRegistry tools;
    llm::ProviderFactoryRegistry providers;
    std::shared_ptr<testing::ScriptedProviderScript> script;
    // The model catalog presets borrows; declared first so it outlives it.
    std::unique_ptr<config::ProvidersConfig> catalog;
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
    // Signal subscriptions: tests register channels on it before Start().
    agent::SignalRouter signal_router;
    // Set by tests before Start(): what the service resolves sessions with.
    config::PermissionsConfig service_permissions;
    tools::PermissionAdvisorRegistry permission_advisors;
};

v1::ClientEvent ChatTurn(const std::string& session_id, const std::string& text) {
    v1::ClientEvent event;
    event.set_session_id(session_id);
    event.mutable_chat_turn()->set_text(text);
    return event;
}

// S5.2: a bare stream drop now detaches rather than terminates, so tests that
// need the session actually GONE (resume-from-store, delete-after-close) must
// say so explicitly on the wire.
v1::ClientEvent EndSession(const std::string& session_id, v1::EndSession::Mode mode) {
    v1::ClientEvent event;
    event.set_session_id(session_id);
    event.mutable_end_session()->set_mode(mode);
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

// Reads until the turn terminator, collecting each event's payload case --
// the shape two streams' deliveries are asserted equal on. Per-subscriber
// notices (attach metadata, resync) are excluded: they are addressed to one
// stream, not part of the session's content order. Returns empty if the
// stream dies before the terminator.
template <typename Stream>
std::vector<v1::ServerEvent::PayloadCase> ReadUntilTurnCompleted(Stream& stream) {
    std::vector<v1::ServerEvent::PayloadCase> cases;
    v1::ServerEvent event;
    while (stream.Read(&event)) {
        const auto payload = event.payload_case();
        if (payload == v1::ServerEvent::kSessionMetadata ||
            payload == v1::ServerEvent::kResyncRequired) {
            continue;
        }
        cases.push_back(payload);
        if (payload == v1::ServerEvent::kTurnCompleted) return cases;
    }
    return {};
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

// --- E5a test commands: one of each display mode, one of each kind ---------
//
// These live here rather than in fakes.hpp because their whole purpose is the
// Display()/ViewHint()/kind surface the wire tests below assert on.

class OpenViewCommand : public commands::HarnessCommand {
public:
    std::string_view Name() const override { return "openview"; }
    std::string_view Summary() const override { return "opens a view"; }
    std::string_view Usage() const override { return "/openview"; }
    bool ReadOnly() const override { return true; }
    commands::CommandDisplay Display() const override {
        return commands::CommandDisplay::kView;
    }
    std::string_view ViewHint() const override { return "sessions"; }
    commands::CommandResult Execute(commands::CommandContext&,
                                    const commands::CommandArgs&) override {
        return commands::CommandResult::Ok("session list requested");
    }
};

class SilentOkCommand : public commands::HarnessCommand {
public:
    std::string_view Name() const override { return "quietok"; }
    std::string_view Summary() const override { return "succeeds silently"; }
    std::string_view Usage() const override { return "/quietok"; }
    bool ReadOnly() const override { return true; }
    commands::CommandDisplay Display() const override {
        return commands::CommandDisplay::kSilent;
    }
    commands::CommandResult Execute(commands::CommandContext&,
                                    const commands::CommandArgs&) override {
        return commands::CommandResult::Ok("did a thing");
    }
};

// A View-displaying command that FAILS: the dispatcher must downgrade the
// result to the status line -- a failed view request has nothing to show.
class FailingViewCommand : public commands::HarnessCommand {
public:
    std::string_view Name() const override { return "brokenview"; }
    std::string_view Summary() const override { return "fails with a view display"; }
    std::string_view Usage() const override { return "/brokenview"; }
    bool ReadOnly() const override { return true; }
    commands::CommandDisplay Display() const override {
        return commands::CommandDisplay::kView;
    }
    std::string_view ViewHint() const override { return "sessions"; }
    commands::CommandResult Execute(commands::CommandContext&,
                                    const commands::CommandArgs&) override {
        return commands::CommandResult::Error("nope");
    }
};

// The prompt-shorthand contract, exactly as a real one would behave: Execute
// submits the interpolated prompt as the user turn and answers with an empty
// kOk. The reply arrives as ordinary turn output, never as a CommandResult.
class InterpolatingShorthand : public commands::PromptShorthandCommand {
public:
    std::string_view Name() const override { return "review"; }
    std::string_view Summary() const override { return "asks for a review"; }
    std::string_view Usage() const override { return "/review <topic>"; }
    commands::CommandResult Execute(commands::CommandContext& context,
                                    const commands::CommandArgs& args) override {
        std::string prompt = "review note:";
        for (const auto& word : args.positional) prompt += " " + word;
        if (!context.session().SubmitTurn(prompt)) {
            return commands::CommandResult::Error("the turn was refused");
        }
        return commands::CommandResult::Ok("submitted for review");
    }
};

// Dispatches one command invocation on an open stream and returns the matching
// CommandResult. Reads to end of stream afterwards via DrainAndFinish semantics
// at the call site.
template <typename Stream>
std::optional<v1::CommandResult> RunCommand(Stream& stream, const std::string& session_id,
                                            const std::string& name,
                                            const std::vector<std::string>& positional = {}) {
    v1::ClientEvent invocation;
    invocation.set_session_id(session_id);
    auto* command = invocation.mutable_command();
    command->set_request_id("cmd-1");
    command->set_name(name);
    for (const auto& word : positional) {
        command->mutable_args()->add_positional(word);
    }
    REQUIRE(stream->Write(invocation));

    v1::ServerEvent event;
    while (stream->Read(&event)) {
        if (event.has_command_result() && event.command_result().request_id() == "cmd-1") {
            return event.command_result();
        }
    }
    return std::nullopt;
}

// Loopback stand-in for the System One endpoint, one pre-canned body per
// accepted connection (the tools_web_test LoopbackServer shape, kept
// file-local like every other helper in this suite).
class JevStubServer {
public:
    explicit JevStubServer(std::vector<std::string> bodies)
        : responses_(std::move(bodies)) {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(listen_fd_ >= 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        REQUIRE(::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        REQUIRE(::listen(listen_fd_, 1) == 0);
        socklen_t length = sizeof(address);
        REQUIRE(::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&address), &length) == 0);
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this] { Serve(); });
    }

    ~JevStubServer() {
        if (listen_fd_ >= 0) ::close(listen_fd_);
        if (thread_.joinable()) thread_.join();
    }

    std::string url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/"; }

private:
    void Serve() {
        timeval tv{10, 0};
        (void)::setsockopt(listen_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        for (const auto& body : responses_) {
            int fd = ::accept(listen_fd_, nullptr, nullptr);
            if (fd < 0) return;
            std::string request;
            char buffer[8192];
            while (request.find("\r\n\r\n") == std::string::npos) {
                ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
                if (n <= 0) break;
                request.append(buffer, static_cast<std::size_t>(n));
            }
            std::string head = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n";
            head += "Content-Length: " + std::to_string(body.size()) + "\r\n";
            head += "Connection: close\r\n\r\n";
            std::string bytes = head + body;
            std::size_t sent = 0;
            while (sent < bytes.size()) {
                ssize_t n = ::send(fd, bytes.data() + sent, bytes.size() - sent, 0);
                if (n <= 0) break;
                sent += static_cast<std::size_t>(n);
            }
            ::close(fd);
        }
    }

    int listen_fd_ = -1;
    int port_ = 0;
    std::vector<std::string> responses_;
    std::thread thread_;
};

// The captured live responses, as the strings the stub answers with.
std::string JevFixtureBody(const std::string& name) {
    std::ifstream stream(std::filesystem::path(TYPESAFE_FIXTURE_DIR) / name);
    REQUIRE(stream.good());
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
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

TEST_CASE("set_run_mode applies without a turn boundary", "[session_service][run_mode]") {
    // The wire path end to end: the event must NOT queue for the worker --
    // a mid-turn press has to gate the running turn's next tool call. The
    // turn here is parked on a permission prompt (the state a shift+tab to
    // bypass is usually pressed in); the mode flip must land while the
    // worker is parked, so the SAME turn's next gated call runs unasked --
    // and the confirming SessionMetadataEvent reaches the stream without a
    // TurnCompleted in between.
    ServiceFixture fixture;
    auto tool =
        std::make_unique<testing::FakeTool>("bash", tools::ToolCategory::Execute, "ran it");
    fixture.tools.Register(std::move(tool));
    fixture.Script(testing::ToolUseResponse("call-1", "bash", {{"command", "make"}}));
    fixture.Script(testing::ToolUseResponse("call-2", "bash", {{"command", "make"}}));
    fixture.Script(testing::TextResponse("built"));
    fixture.Write("permissions.json", nlohmann::json{{"fallback", {{"execute", "ask"}}}}.dump());
    fixture.Start();

    const std::string session_id = fixture.CreateSession();

    grpc::ClientContext context;
    auto stream = fixture.stub->OpenSession(&context);
    REQUIRE(stream->Write(ChatTurn(session_id, "build it")));

    std::string request_id;
    v1::ServerEvent event;
    while (stream->Read(&event)) {
        if (event.has_permission_request()) {
            request_id = event.permission_request().request_id();
            break;
        }
    }
    REQUIRE_FALSE(request_id.empty());  // the worker is parked on the prompt

    // The press, as a raw wire event.
    v1::ClientEvent press;
    press.set_session_id(session_id);
    press.mutable_set_run_mode()->set_run_mode(v1::PERMISSION_RUN_MODE_BYPASS);
    REQUIRE(stream->Write(press));

    // The confirming metadata event arrives while the turn is STILL RUNNING
    // -- the old queued switch would have emitted nothing until the turn
    // ended.
    bool confirmed = false;
    while (stream->Read(&event)) {
        if (event.has_session_metadata()) {
            if (event.session_metadata().permission_mode() == "bypass") confirmed = true;
            break;
        }
        if (event.has_turn_completed()) break;  // must NOT be the trigger
    }
    CHECK(confirmed);

    // Answer the one open prompt; the turn's second gated call must not ask.
    v1::ClientEvent answer;
    answer.set_session_id(session_id);
    auto* response = answer.mutable_permission_response();
    response->set_request_id(request_id);
    response->set_decision(v1::PERMISSION_DECISION_ALLOW);
    REQUIRE(stream->Write(answer));

    v1::ServerEvent terminal;
    bool second_ask = false;
    while (stream->Read(&terminal)) {
        if (terminal.has_permission_request()) second_ask = true;
        if (terminal.has_stream_event() && terminal.stream_event().has_message_stop()) break;
        if (terminal.has_error()) break;
    }
    (void)DrainAndFinish(stream);
    CHECK_FALSE(second_ask);
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

TEST_CASE("CreateSession: a remembered selection falls back instead of refusing",
          "[session_service][last_selection]") {
    ServiceFixture fixture;
    fixture.Start();
    const auto create = [&](const std::string& agent, const std::string& model, bool remembered,
                            v1::CreateSessionResponse* response) {
        grpc::ClientContext context;
        v1::CreateSessionRequest request;
        request.set_project_root((fixture.dir / "work").string());
        request.set_agent_preset(agent);
        request.set_model(model);
        request.set_remembered_selection(remembered);
        return fixture.stub->CreateSession(&context, request, response);
    };

    // A choice made NOW that does not resolve is refused.
    {
        v1::CreateSessionResponse response;
        CHECK_FALSE(create("main", "main/gone", /*remembered=*/false, &response).ok());
    }
    // The same pick REMEMBERED starts without the stale model; the response
    // says what ran, so the client can forget the rest.
    {
        v1::CreateSessionResponse response;
        REQUIRE(create("main", "main/gone", /*remembered=*/true, &response).ok());
        CHECK(response.session().agent_preset() == "main");
        CHECK(response.session().model().empty());
    }
    // A stale remembered agent falls back to default_agent_preset.
    {
        v1::CreateSessionResponse response;
        REQUIRE(create("deleted-preset", "", /*remembered=*/true, &response).ok());
        CHECK(response.session().agent_preset() == "main");
    }
    // A remembered pick that still resolves is honored as is.
    {
        v1::CreateSessionResponse response;
        REQUIRE(create("main", "main/test-model", /*remembered=*/true, &response).ok());
        CHECK(response.session().model() == "main/test-model");
    }
}

TEST_CASE("CreateSession with no agent and no usable default names the choices",
          "[session_service][defaults]") {
    ServiceFixture fixture;
    fixture.Write("settings.json", nlohmann::json{{"default_model", "main/test-model"}}.dump());
    fixture.Write("agents/plan.md", "---\nname: plan\nallowed_tools: [\"*\"]\n---\nPlan.\n");
    fixture.Start();

    grpc::ClientContext context;
    v1::CreateSessionRequest request;
    request.set_project_root((fixture.dir / "work").string());
    v1::CreateSessionResponse response;
    const auto status = fixture.stub->CreateSession(&context, request, &response);
    CHECK(status.error_code() == grpc::StatusCode::FAILED_PRECONDITION);
    CHECK(status.error_message().find("main, plan") != std::string::npos);

    // The listing says there is no default, so the client knows to ask.
    grpc::ClientContext list_context;
    v1::ListAgentPresetsRequest list_request;
    v1::ListAgentPresetsResponse listed;
    REQUIRE(fixture.stub->ListAgentPresets(&list_context, list_request, &listed).ok());
    CHECK(listed.default_agent_preset().empty());
    CHECK(listed.presets_size() == 2);
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

TEST_CASE("answering a permission prompt dismisses every other subscriber's copy",
          "[session_service][S5]") {
    // S5.3: two clients attached to one session both see the prompt; whichever
    // answers first resolves it (Resolve()'s winner tracking), and the OTHER
    // gets PromptResolvedEvent so its modal does not sit on a dead question.
    ServiceFixture fixture;
    auto tool = std::make_unique<testing::FakeTool>("bash", tools::ToolCategory::Execute, "ran it");
    tool->set_disclosure({"run make", {{"bash:/usr/bin/make", "/usr/bin/make *"}}, true});
    fixture.tools.Register(std::move(tool));
    fixture.Script(testing::ToolUseResponse("call-1", "bash", {{"command", "make"}}));
    fixture.Script(testing::TextResponse("built"));
    fixture.Write("permissions.json", nlohmann::json{{"fallback", {{"execute", "ask"}}}}.dump());
    fixture.Start();

    const std::string session_id = fixture.CreateSession();

    grpc::ClientContext context_a;
    auto stream_a = fixture.stub->OpenSession(&context_a);

    // B attaches BEFORE A starts the turn. A later subscriber is replayed
    // nothing it missed (EventBroadcaster::Subscribe hands it a late_attach
    // resync instead), so a B attaching after the prompt went out would sit
    // in its Read below forever. Attaching after the turn only ever worked
    // because the scripted turn usually took longer than B's attach round
    // trip; under load it did not (~1% of runs on macOS, ~9% on Linux).
    // Reading B's SessionMetadataEvent proves its subscription is live: the
    // server subscribes the stream before emitting it.
    grpc::ClientContext context_b;
    auto stream_b = fixture.stub->OpenSession(&context_b);
    v1::ClientEvent attach_b;
    attach_b.set_session_id(session_id);
    attach_b.mutable_attach();
    REQUIRE(stream_b->Write(attach_b));
    v1::ServerEvent metadata_b;
    REQUIRE(stream_b->Read(&metadata_b));  // the attach-time SessionMetadataEvent

    REQUIRE(stream_a->Write(ChatTurn(session_id, "build it")));

    // Stream A drives the turn and sees the permission_request; stream B sees
    // the same request (broadcast) but never answers it.
    std::string request_id;
    v1::ServerEvent event_a;
    while (stream_a->Read(&event_a)) {
        if (event_a.has_permission_request()) {
            request_id = event_a.permission_request().request_id();
            break;
        }
    }
    REQUIRE_FALSE(request_id.empty());

    v1::ServerEvent event_b;
    bool b_saw_request = false;
    while (stream_b->Read(&event_b)) {
        if (event_b.has_permission_request()) {
            CHECK(event_b.permission_request().request_id() == request_id);
            b_saw_request = true;
            break;
        }
    }
    REQUIRE(b_saw_request);

    v1::ClientEvent answer;
    answer.set_session_id(session_id);
    auto* response = answer.mutable_permission_response();
    response->set_request_id(request_id);
    response->set_decision(v1::PERMISSION_DECISION_ALLOW);
    REQUIRE(stream_a->Write(answer));

    // B, having never answered, sees PromptResolvedEvent for the same id.
    bool b_saw_resolved = false;
    while (stream_b->Read(&event_b)) {
        if (event_b.has_prompt_resolved()) {
            CHECK(event_b.prompt_resolved().request_id() == request_id);
            b_saw_resolved = true;
            break;
        }
    }
    CHECK(b_saw_resolved);

    (void)DrainAndFinish(stream_a);
    (void)DrainAndFinish(stream_b);
}

TEST_CASE("a duplicate permission answer from a second client is a harmless no-op",
          "[session_service][S5]") {
    // The other half of first-answer-wins: B answering AFTER A already did
    // must not crash, double-broadcast, or override A's decision.
    ServiceFixture fixture;
    auto tool = std::make_unique<testing::FakeTool>("bash", tools::ToolCategory::Execute, "ran it");
    tool->set_disclosure({"run make", {{"bash:/usr/bin/make", "/usr/bin/make *"}}, true});
    fixture.tools.Register(std::move(tool));
    fixture.Script(testing::ToolUseResponse("call-1", "bash", {{"command", "make"}}));
    fixture.Script(testing::TextResponse("built"));
    fixture.Write("permissions.json", nlohmann::json{{"fallback", {{"execute", "ask"}}}}.dump());
    fixture.Start();

    const std::string session_id = fixture.CreateSession();

    grpc::ClientContext context_a;
    auto stream_a = fixture.stub->OpenSession(&context_a);
    REQUIRE(stream_a->Write(ChatTurn(session_id, "build it")));

    std::string request_id;
    v1::ServerEvent event_a;
    while (stream_a->Read(&event_a)) {
        if (event_a.has_permission_request()) {
            request_id = event_a.permission_request().request_id();
            break;
        }
    }
    REQUIRE_FALSE(request_id.empty());

    v1::ClientEvent allow;
    allow.set_session_id(session_id);
    allow.mutable_permission_response()->set_request_id(request_id);
    allow.mutable_permission_response()->set_decision(v1::PERMISSION_DECISION_ALLOW);
    REQUIRE(stream_a->Write(allow));

    // A retransmit (a duplicate DENY): must not flip the already-granted
    // decision, and must not crash dispatch. There is only one stream, so this
    // just proves the server does not choke on it -- the winner-only-broadcast
    // behavior itself is covered above with a second, non-answering client.
    v1::ClientEvent deny;
    deny.set_session_id(session_id);
    deny.mutable_permission_response()->set_request_id(request_id);
    deny.mutable_permission_response()->set_decision(v1::PERMISSION_DECISION_DENY);
    REQUIRE(stream_a->Write(deny));

    bool saw_finished_tool = false;
    while (stream_a->Read(&event_a)) {
        if (event_a.has_tool_execution() && !event_a.tool_execution().started()) {
            saw_finished_tool = true;
            break;
        }
    }
    (void)DrainAndFinish(stream_a);
    CHECK(saw_finished_tool);  // ran, i.e. the first answer (ALLOW) is the one that stuck
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

TEST_CASE("closing the stream detaches rather than terminates", "[session_service][S5]") {
    // S5.2: a bare drop is now the graceful form of leaving, not an implicit
    // terminate -- a crashed terminal must not destroy in-flight work. The
    // session stays live in the registry with zero subscribers.
    ServiceFixture fixture;
    fixture.Script(testing::TextResponse("hi"));
    fixture.Script(testing::TextResponse("and again"));
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

    const auto deadline = std::chrono::steady_clock::now() + 500ms;
    while (session->has_subscribers() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    CHECK_FALSE(session->terminated());
    CHECK_FALSE(session->has_subscribers());
    CHECK(fixture.sessions->Find(session_id) == session);
}

TEST_CASE("an explicit TERMINATE ends the session for good", "[session_service][S5]") {
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
        REQUIRE(stream->Write(EndSession(session_id, v1::EndSession::TERMINATE)));
        (void)DrainAndFinish(stream);
    }

    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!session->terminated() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    CHECK(session->terminated());
    CHECK(fixture.sessions->Find(session_id) == nullptr);
}

TEST_CASE("EndSession as the very first event on a stream still detaches",
          "[session_service][S5]") {
    // session_id and end_session are both fields on one ClientEvent, so a
    // client can name the session and leave it in the same message -- the
    // read loop must special-case first-event EndSession exactly like every
    // later one, not silently swallow it via Dispatch (which has no case for
    // it and would leave the stream sitting in the read loop instead).
    ServiceFixture fixture;
    fixture.Start();

    const std::string session_id = fixture.CreateSession();
    auto session = fixture.sessions->Find(session_id);
    REQUIRE(session != nullptr);

    grpc::ClientContext context;
    auto stream = fixture.stub->OpenSession(&context);
    REQUIRE(stream->Write(EndSession(session_id, v1::EndSession::DETACH)));
    const auto status = DrainAndFinish(stream);
    CHECK(status.ok());
    CHECK_FALSE(session->terminated());
}

TEST_CASE("client::SessionClient::Stream::Detach leaves the session running",
          "[session_service][S5]") {
    // The real client-side call S5.4 adds: quit's teardown uses this instead
    // of the destructive Close(). Driven through the real channel, unlike the
    // proto-level EndSession(...) helper other S5 tests use directly.
    ServiceFixture fixture;
    fixture.Script(testing::TextResponse("hi"));
    fixture.Start();

    const std::string session_id = fixture.CreateSession();
    auto session = fixture.sessions->Find(session_id);
    REQUIRE(session != nullptr);

    minimal::client::SessionClient client(rpc::SocketTarget(fixture.socket));
    auto stream = client.MakeStream([](const v1::ServerEvent&) {});
    REQUIRE(stream->Attach(session_id).has_value());
    stream->Resume();

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!session->has_subscribers() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    REQUIRE(session->has_subscribers());

    stream->Detach();

    CHECK(stream->status().ok());
    CHECK_FALSE(session->terminated());
    CHECK_FALSE(session->has_subscribers());
    CHECK(fixture.sessions->Find(session_id) == session);
}

TEST_CASE("Detach before Resume reaps the stream instead of hanging",
          "[session_service][S5]") {
    // The bug: Detach() on a stream whose Resume() never ran skipped its
    // whole bounded-wait-then-cancel block (nothing to wait on -- no writer
    // thread exists yet) and fell straight to a raw Finish(), with no cancel
    // ever issued. The naming event this stream's Attach() queued was never
    // sent (no writer thread ever ran to send it), so the server's handler
    // sat forever in its own Read() waiting for it, and Finish() blocked
    // right along with it -- quit hung forever whenever a subagent spawn
    // (AttachTab) raced a shutdown (DetachAllTabs) landing between
    // registering the tab and calling Resume() (the exact window
    // AttachTab's own comment names).
    ServiceFixture fixture;
    fixture.Script(testing::TextResponse("hi"));
    fixture.Start();

    const std::string session_id = fixture.CreateSession();

    auto client = std::make_shared<minimal::client::SessionClient>(
        rpc::SocketTarget(fixture.socket));
    auto stream = client->MakeStream([](const v1::ServerEvent&) {});
    REQUIRE(stream->Attach(session_id).has_value());
    // Deliberately no stream->Resume() here.

    // A hang is the failure mode under test, so this runs on a detached
    // thread the callable owns by shared_ptr (see deadline.hpp) rather than
    // blocking the test itself if the bug regresses.
    auto result = minimal::testing::RunInBackground([client, stream] {
        stream->Detach();
        return true;
    });
    REQUIRE(result.WaitFor(std::chrono::seconds(2)));
}

TEST_CASE("Resume after Detach never launches threads on an already-closed stream",
          "[session_service][S5]") {
    // The other half of the same race: if Resume() runs AFTER a concurrent
    // Detach() already closed the stream, it used to launch reader_/writer_
    // anyway (the old resumed_.exchange(true) never checked closed_). On a
    // stream whose last owner drops it right after, ~Stream's Close() is
    // then a no-op (already latched by Detach()) and the destructor destroys
    // the now-joinable thread members: std::terminate.
    ServiceFixture fixture;
    fixture.Script(testing::TextResponse("hi"));
    fixture.Start();

    const std::string session_id = fixture.CreateSession();

    minimal::client::SessionClient client(rpc::SocketTarget(fixture.socket));
    auto stream = client.MakeStream([](const v1::ServerEvent&) {});
    REQUIRE(stream->Attach(session_id).has_value());

    stream->Detach();
    // The never-resumed path cancels before Finish() (unlike the graceful,
    // writer-flushes-EndSession path a resumed stream takes) -- CANCELLED is
    // the correct, expected status here, not OK. The real assertion is that
    // this line was ever reached at all: pre-fix, Detach() blocked forever.
    CHECK_FALSE(stream->status().ok());

    stream->Resume();   // must be a safe no-op, never launch threads on a closed stream
    CHECK_FALSE(stream->resumed());
    // The real assertion is that falling off the end of this test destroys
    // `stream` (and `client`) with no joinable thread ever created --
    // std::terminate would otherwise take the whole test binary down.
}

TEST_CASE("the last client detaching signals shutdown", "[session_service]") {
    // No daemon: the server is refcounted by attached clients and goes away when
    // the last one leaves.
    ServiceFixture fixture;
    fixture.Script(testing::TextResponse("hi"));
    fixture.Script(testing::TextResponse("and again"));
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
    // (SuggestPaths used to be in this test; it is implemented now -- see the
    // [mention] cases below. PluginCall is implemented too -- E5's generic
    // channel; with nothing registered under the requested name it answers
    // NOT_FOUND, and with no registry at all UNAVAILABLE.)
    ServiceFixture fixture;
    fixture.Start();

    {
        grpc::ClientContext context;
        v1::PluginCallRequest request;
        request.set_plugin_name("no-such-plugin");
        v1::PluginCallResponse response;
        CHECK(fixture.stub->PluginCall(&context, request, &response).error_code() ==
              grpc::StatusCode::NOT_FOUND);
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

TEST_CASE("SuggestPaths answers for the session's project root, relative paths only",
          "[session_service][mention]") {
    ServiceFixture fixture;
    fixture.Start();

    const std::string session_id = fixture.CreateSession();
    std::filesystem::create_directories(fixture.dir / "work" / "src");
    { std::ofstream out(fixture.dir / "work" / "src" / "main.cpp"); out << "x"; }
    { std::ofstream out(fixture.dir / "work" / "README.md"); out << "y"; }

    grpc::ClientContext context;
    v1::SuggestPathsRequest request;
    request.set_session_id(session_id);
    request.set_prefix("");
    request.set_limit(50);
    v1::SuggestPathsResponse response;
    REQUIRE(fixture.stub->SuggestPaths(&context, request, &response).ok());

    // Sorted, relative to project_root, directories carrying the descending '/'.
    REQUIRE(response.paths_size() == 2);
    CHECK(response.paths(0) == "README.md");
    CHECK(response.paths(1) == "src/");
}

TEST_CASE("SuggestPaths falls back to the stored row for a terminated session",
          "[session_service][mention]") {
    // Same rule as GetMessages: a terminated session still answers. A tab may
    // outlive the session object that backed it mid-keystroke.
    ServiceFixture fixture;
    fixture.Start();

    const std::string session_id = fixture.CreateSession();
    fixture.sessions->Terminate(session_id);  // session object gone; row remains

    grpc::ClientContext context;
    v1::SuggestPathsRequest request;
    request.set_session_id(session_id);
    v1::SuggestPathsResponse response;
    REQUIRE(fixture.stub->SuggestPaths(&context, request, &response).ok());
    CHECK(response.paths_size() == 0);  // the (empty) work dir, not an error
}

TEST_CASE("SuggestPaths for an unknown session is NOT_FOUND, not an empty page",
          "[session_service][mention]") {
    ServiceFixture fixture;
    fixture.Start();

    grpc::ClientContext context;
    v1::SuggestPathsRequest request;
    request.set_session_id("01ARZ3NDEKTSV4RRFFQ69G5FAV");
    v1::SuggestPathsResponse response;
    CHECK(fixture.stub->SuggestPaths(&context, request, &response).error_code() ==
          grpc::StatusCode::NOT_FOUND);
}

TEST_CASE("SuggestPaths scopes to a bare project_root for a sessionless tab",
          "[session_service][mention]") {
    // A sessionless tab owns no session row until its first message; the
    // root scope is how '@' autocomplete stays alive on it.
    ServiceFixture fixture;
    fixture.Start();

    std::filesystem::create_directories(fixture.dir / "work" / "src");
    { std::ofstream out(fixture.dir / "work" / "README.md"); out << "y"; }

    grpc::ClientContext context;
    v1::SuggestPathsRequest request;
    request.set_project_root((fixture.dir / "work").string());
    request.set_prefix("");
    request.set_limit(50);
    v1::SuggestPathsResponse response;
    REQUIRE(fixture.stub->SuggestPaths(&context, request, &response).ok());

    REQUIRE(response.paths_size() == 2);
    CHECK(response.paths(0) == "README.md");
    CHECK(response.paths(1) == "src/");
}

TEST_CASE("SuggestPaths rejects an unknown project_root as NOT_FOUND, and "
          "ambiguous scopes as INVALID_ARGUMENT",
          "[session_service][mention]") {
    ServiceFixture fixture;
    fixture.Start();

    const std::string session_id = fixture.CreateSession();

    // A root that is not a directory is NOT_FOUND -- the same honest answer
    // as an unknown session, not an empty page that reads as "no matches".
    {
        grpc::ClientContext context;
        v1::SuggestPathsRequest request;
        request.set_project_root((fixture.dir / "nope").string());
        v1::SuggestPathsResponse response;
        CHECK(fixture.stub->SuggestPaths(&context, request, &response).error_code() ==
              grpc::StatusCode::NOT_FOUND);
    }
    // Exactly one scope must be named: both is a malformed request.
    {
        grpc::ClientContext context;
        v1::SuggestPathsRequest request;
        request.set_session_id(session_id);
        request.set_project_root((fixture.dir / "work").string());
        v1::SuggestPathsResponse response;
        CHECK(fixture.stub->SuggestPaths(&context, request, &response).error_code() ==
              grpc::StatusCode::INVALID_ARGUMENT);
    }
    // ... and so is neither.
    {
        grpc::ClientContext context;
        v1::SuggestPathsRequest request;
        v1::SuggestPathsResponse response;
        CHECK(fixture.stub->SuggestPaths(&context, request, &response).error_code() ==
              grpc::StatusCode::INVALID_ARGUMENT);
    }
}

TEST_CASE("a turn with @-mentions persists the verbatim text plus framed blocks",
          "[session_service][mention]") {
    ServiceFixture fixture;
    fixture.Script(testing::TextResponse("looked"));
    fixture.Start();

    const std::string session_id = fixture.CreateSession();
    { std::ofstream out(fixture.dir / "work" / "notes.md"); out << "file body"; }

    grpc::ClientContext open_context;
    auto stream = fixture.stub->OpenSession(&open_context);
    REQUIRE(stream->Write(ChatTurn(session_id, "see @notes.md")));
    REQUIRE(WaitForTurnCompleted(*stream));
    (void)DrainAndFinish(stream);

    // The persisted user message IS the expanded one -- the transcript records
    // what the model saw. Blocks, not a rewritten single text: the original
    // sentence survives verbatim as its own block.
    grpc::ClientContext context;
    v1::GetMessagesRequest request;
    request.set_session_id(session_id);
    v1::GetMessagesResponse response;
    REQUIRE(fixture.stub->GetMessages(&context, request, &response).ok());
    REQUIRE(response.messages_size() == 2);
    const auto& user = response.messages(1);
    REQUIRE(user.message().role() == v1::ROLE_USER);
    REQUIRE(user.message().content_size() == 2);
    CHECK(user.message().content(0).text().text() == "see @notes.md");
    CHECK(user.message().content(1).text().text() ==
          "<file path=\"notes.md\">\nfile body\n</file>");
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

TEST_CASE("ListSessions.is_live reflects the live registry, not the stored row",
          "[session_service][S5]") {
    ServiceFixture fixture;
    fixture.Script(testing::TextResponse("hi"));
    fixture.Start();

    const std::string live_id = fixture.CreateSession();
    const std::string dead_id = fixture.CreateSession();
    fixture.sessions->Terminate(dead_id);
    REQUIRE(fixture.sessions->Find(dead_id) == nullptr);
    REQUIRE(fixture.sessions->Find(live_id) != nullptr);

    grpc::ClientContext context;
    v1::ListSessionsRequest request;
    request.set_include_all_dirs(true);
    v1::ListSessionsResponse response;
    REQUIRE(fixture.stub->ListSessions(&context, request, &response).ok());

    bool saw_live = false, saw_dead = false;
    for (const auto& summary : response.sessions()) {
        if (summary.session_id() == live_id) {
            CHECK(summary.is_live());
            saw_live = true;
        } else if (summary.session_id() == dead_id) {
            CHECK_FALSE(summary.is_live());
            saw_dead = true;
        }
    }
    CHECK(saw_live);
    CHECK(saw_dead);
}

TEST_CASE("GetSession answers one stored row with registry liveness",
          "[session_service][S5]") {
    // The startup --resume probe's primitive: one id in, the row out, with the
    // same is_live answer ListSessions gives -- the caller's attach-vs-resume
    // decision reads it. A subagent row answers normally (parent_session_id
    // set); that passthrough is the store layer's ToProto, covered there.
    ServiceFixture fixture;
    fixture.Script(testing::TextResponse("hi"));
    fixture.Start();

    const std::string live_id = fixture.CreateSession();
    const std::string dead_id = fixture.CreateSession();
    fixture.sessions->Terminate(dead_id);

    grpc::ClientContext live_context;
    v1::GetSessionRequest live_request;
    live_request.set_session_id(live_id);
    v1::SessionSummary live_summary;
    REQUIRE(fixture.stub->GetSession(&live_context, live_request, &live_summary).ok());
    CHECK(live_summary.session_id() == live_id);
    CHECK(live_summary.is_live());

    grpc::ClientContext dead_context;
    v1::GetSessionRequest dead_request;
    dead_request.set_session_id(dead_id);
    v1::SessionSummary dead_summary;
    REQUIRE(fixture.stub->GetSession(&dead_context, dead_request, &dead_summary).ok());
    CHECK_FALSE(dead_summary.is_live());
}

TEST_CASE("GetSession refuses unknown and empty ids", "[session_service][S5]") {
    ServiceFixture fixture;
    fixture.Start();

    grpc::ClientContext unknown_context;
    v1::GetSessionRequest unknown_request;
    unknown_request.set_session_id("01ARZ3NDEKTSV4RRFFQ69G5FAV");
    v1::SessionSummary unknown_summary;
    CHECK(fixture.stub->GetSession(&unknown_context, unknown_request, &unknown_summary)
              .error_code() == grpc::StatusCode::NOT_FOUND);

    grpc::ClientContext empty_context;
    v1::GetSessionRequest empty_request;  // no session_id
    v1::SessionSummary empty_summary;
    CHECK(fixture.stub->GetSession(&empty_context, empty_request, &empty_summary)
              .error_code() == grpc::StatusCode::INVALID_ARGUMENT);
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

TEST_CASE("a half-open stream cannot block shutdown past the grace deadline",
          "[session_service][invariant]") {
    // THE DEADLINE on shutdown: an unbounded graceful shutdown let a half-open
    // stream -- a client whose process died without closing its socket --
    // block Server::Wait() and therefore process exit indefinitely. In-flight
    // RPCs get the grace window; past it they are cancelled. The fixture's
    // server uses the production 30 s grace, so this test swaps in its own
    // with a deadline short enough to assert on.
    ServiceFixture fixture;
    fixture.Script(testing::TextResponse("hi"));
    fixture.Script(testing::TextResponse("and again"));
    fixture.Start();
    const std::string session_id = fixture.CreateSession();

    fixture.server->Shutdown();
    const auto grace_socket = fixture.dir / "grace.sock";
    auto server =
        std::make_unique<rpc::Server>(*fixture.service, grace_socket, std::chrono::seconds{1});
    REQUIRE(server->listening());

    auto channel = grpc::CreateChannel(rpc::SocketTarget(grace_socket),
                                       grpc::InsecureChannelCredentials());
    auto stub = v1::SessionService::NewStub(channel);
    grpc::ClientContext context;
    auto stream = stub->OpenSession(&context);
    v1::ClientEvent naming;
    naming.set_session_id(session_id);
    naming.mutable_attach();
    REQUIRE(stream->Write(naming));  // attached; the handler parks in Read() forever

    auto waited = testing::RunInBackground([&server] {
        server->Wait();
        return true;
    });
    std::this_thread::sleep_for(100ms);
    CHECK_FALSE(waited.Ready());  // still serving

    server->RequestShutdown();
    // Graceful alone would wait for the open stream; the deadline cancels it.
    REQUIRE(waited.WaitFor(10s));
    (void)DrainAndFinish(stream);
}

TEST_CASE("a command invocation is answered while the subsystem is missing",
          "[session_service]") {
    ServiceFixture fixture;
    fixture.Script(testing::TextResponse("hi"));
    fixture.Script(testing::TextResponse("and again"));
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

TEST_CASE("ListCommands carries name, summary, usage, kind, display and read_only",
          "[session_service][e5]") {
    // The enumeration RPC EXTENSIBILITY.md 4.3 ruled into existence: without it
    // plugin commands are invisible to /-autocomplete. Kind is derived from the
    // command's TYPE, not a flag -- the same rule the busy-check runs on.
    ServiceFixture fixture;
    fixture.commands.Register(std::make_unique<OpenViewCommand>());
    fixture.commands.Register(std::make_unique<SilentOkCommand>());
    fixture.commands.Register(std::make_unique<InterpolatingShorthand>());
    fixture.Script(testing::TextResponse("hi"));
    fixture.Start();

    grpc::ClientContext context;
    v1::ListCommandsResponse response;
    REQUIRE(fixture.stub->ListCommands(&context, v1::ListCommandsRequest{}, &response).ok());

    std::map<std::string, const v1::CommandInfo*> by_name;
    for (const auto& info : response.commands()) by_name[info.name()] = &info;
    REQUIRE(by_name.count("openview") == 1);
    REQUIRE(by_name.count("quietok") == 1);
    REQUIRE(by_name.count("review") == 1);

    CHECK(by_name["openview"]->kind() == v1::COMMAND_KIND_HARNESS);
    CHECK(by_name["openview"]->read_only());

    CHECK(by_name["review"]->kind() == v1::COMMAND_KIND_PROMPT_SHORTHAND);
    CHECK_FALSE(by_name["review"]->read_only());

    CHECK(by_name["openview"]->summary() == "opens a view");
}

TEST_CASE("a CommandResult carries its display mode, and an error degrades to the status line",
          "[session_service][e5]") {
    ServiceFixture fixture;
    fixture.commands.Register(std::make_unique<OpenViewCommand>());
    fixture.commands.Register(std::make_unique<FailingViewCommand>());
    fixture.commands.Register(std::make_unique<SilentOkCommand>());
    fixture.Script(testing::TextResponse("hi"));
    fixture.Start();

    const std::string session_id = fixture.CreateSession();
    grpc::ClientContext context;
    auto stream = fixture.stub->OpenSession(&context);

    const auto view = RunCommand(stream, session_id, "openview");
    REQUIRE(view.has_value());
    CHECK(view->status() == v1::COMMAND_STATUS_OK);
    CHECK(view->display() == v1::COMMAND_DISPLAY_VIEW);
    CHECK(view->view_hint() == "sessions");

    const auto silent = RunCommand(stream, session_id, "quietok");
    REQUIRE(silent.has_value());
    CHECK(silent->display() == v1::COMMAND_DISPLAY_SILENT);
    CHECK(silent->view_hint().empty());

    const auto failed = RunCommand(stream, session_id, "brokenview");
    REQUIRE(failed.has_value());
    CHECK(failed->status() == v1::COMMAND_STATUS_ERROR);
    // Downgraded: errors belong on the notice line even when the command that
    // failed normally opens a view (dispatcher rule, EXTENSIBILITY 4.4).
    CHECK(failed->display() == v1::COMMAND_DISPLAY_STATUS);
    CHECK(failed->view_hint().empty());

    stream->WritesDone();
    v1::ServerEvent discarded;
    while (stream->Read(&discarded)) {
    }
    (void)stream->Finish();
}

TEST_CASE("a prompt shorthand submits exactly the interpolated turn, and a command result "
          "never lands in the store",
          "[session_service][e5][invariant]") {
    // The invariant that makes the taxonomy honest (EXTENSIBILITY 4.4): the
    // ONLY way a command's content reaches the model is a shorthand's submitted
    // turn. Nothing any command answers is appended to history, so a transcript
    // after /review holds exactly two rows -- the interpolated user turn and
    // the model's reply to it.
    ServiceFixture fixture;
    fixture.commands.Register(std::make_unique<OpenViewCommand>());
    fixture.commands.Register(std::make_unique<InterpolatingShorthand>());
    fixture.Script(testing::TextResponse("the review, as a real turn reply"));
    fixture.Start();

    const std::string session_id = fixture.CreateSession();
    grpc::ClientContext context;
    auto stream = fixture.stub->OpenSession(&context);

    const auto view = RunCommand(stream, session_id, "openview");
    REQUIRE(view.has_value());
    CHECK(view->status() == v1::COMMAND_STATUS_OK);
    // An ordinary HarnessCommand's Ok result never means a turn was queued --
    // the headless -p client's stay-attached-and-keep-reading path (Fix 6)
    // must only trigger for a PromptShorthandCommand.
    CHECK_FALSE(view->submitted_turn());

    const auto review = RunCommand(stream, session_id, "review", {"the", "login", "flow"});
    REQUIRE(review.has_value());
    CHECK(review->status() == v1::COMMAND_STATUS_OK);
    // The signal the headless -p client's command branch keys on to stay
    // attached instead of exiting the moment this CommandResult arrives: an
    // Ok result from a PromptShorthandCommand always means the turn it just
    // queued is still running (command_dispatcher.cpp's ExecuteAndShape
    // derives this from dynamic_cast<PromptShorthandCommand*>, never trusted
    // from the command itself).
    CHECK(review->submitted_turn());

    // The submitted turn runs after the command; wait for it to finish before
    // reading the transcript.
    REQUIRE(WaitForTurnCompleted(*stream));
    stream->WritesDone();
    v1::ServerEvent discarded;
    while (stream->Read(&discarded)) {
    }
    (void)stream->Finish();

    const auto page = fixture.db->GetMessagesPage(session_id, std::nullopt, 50);
    REQUIRE(page.messages.size() == 2);
    // Descending by seq: [1] is the model's reply, [0] the submitted turn.
    CHECK(page.messages[1].message.role == llm::Role::User);
    CHECK(page.messages[1].message.content.size() == 1);
    CHECK(std::get<llm::TextBlock>(page.messages[1].message.content[0]).text ==
          "review note: the login flow");
    CHECK(page.messages[0].message.role == llm::Role::Assistant);
}

TEST_CASE("DeleteSession is refused while the session is open, works after close",
          "[session_service][e5]") {
    ServiceFixture fixture;
    fixture.Script(testing::TextResponse("hi"));
    fixture.Script(testing::TextResponse("and again"));
    fixture.Start();

    const std::string session_id = fixture.CreateSession();

    grpc::ClientContext context;
    auto stream = fixture.stub->OpenSession(&context);
    v1::ClientEvent naming;
    naming.set_session_id(session_id);
    naming.mutable_attach();  // attach only: the deletion guard is about the stream
    REQUIRE(stream->Write(naming));
    v1::ServerEvent attached;
    REQUIRE(stream->Read(&attached));  // the attach-time SessionMetadataEvent

    v1::DeleteSessionRequest del;
    del.set_session_id(session_id);
    {
        grpc::ClientContext ctx;
        v1::DeleteSessionResponse response;
        const auto status = fixture.stub->DeleteSession(&ctx, del, &response);
        CHECK(status.error_code() == grpc::StatusCode::FAILED_PRECONDITION);
    }

    // Empty and unknown ids answer INVALID_ARGUMENT / NOT_FOUND, not OK.
    {
        grpc::ClientContext ctx;
        v1::DeleteSessionResponse response;
        v1::DeleteSessionRequest empty;
        CHECK(fixture.stub->DeleteSession(&ctx, empty, &response).error_code() ==
              grpc::StatusCode::INVALID_ARGUMENT);
    }
    {
        grpc::ClientContext ctx;
        v1::DeleteSessionResponse response;
        v1::DeleteSessionRequest ghost;
        ghost.set_session_id("no-such-session");
        CHECK(fixture.stub->DeleteSession(&ctx, ghost, &response).error_code() ==
              grpc::StatusCode::NOT_FOUND);
    }

    // S5.2: closing the stream only detaches, so the row stays undeletable
    // until the session is actually gone -- send an explicit TERMINATE first.
    // Once it is, the row is deletable and the delete cascades to messages
    // (the store-level guarantee the session_store tests cover -- here we
    // assert the RPC wiring only).
    REQUIRE(stream->Write(EndSession(session_id, v1::EndSession::TERMINATE)));
    (void)DrainAndFinish(stream);
    {
        grpc::ClientContext ctx;
        v1::DeleteSessionResponse response;
        REQUIRE(fixture.stub->DeleteSession(&ctx, del, &response).ok());

        grpc::ClientContext again;
        v1::DeleteSessionResponse response2;
        CHECK(fixture.stub->DeleteSession(&again, del, &response2).error_code() ==
              grpc::StatusCode::NOT_FOUND);
    }
}

TEST_CASE("a fresh stream opens with a SessionMetadataEvent carrying the status-bar fields",
          "[session_service][e5]") {
    ServiceFixture fixture;
    // A real window on the model: the `max` half of the status bar's {tokens}
    // resolves from the same place compaction's strategies do.
    fixture.Write(
        "providers.json",
        nlohmann::json{{"providers",
                        {{"main",
                          {{"backend", "fake"},
                           {"url", "http://localhost/v1"},
                           {"models",
                            {{"test-model", {{"context_window_tokens", 48000}}}}}}}}}}
            .dump());
    fixture.Script(testing::TextResponse("hi"));
    fixture.Start();

    const std::string session_id = fixture.CreateSession();

    grpc::ClientContext context;
    auto stream = fixture.stub->OpenSession(&context);
    v1::ClientEvent naming;
    naming.set_session_id(session_id);
    naming.mutable_attach();
    REQUIRE(stream->Write(naming));

    v1::ServerEvent event;
    REQUIRE(stream->Read(&event));
    REQUIRE(event.has_session_metadata());
    const auto& metadata = event.session_metadata();
    CHECK(metadata.session_id() == session_id);
    CHECK(metadata.agent_preset() == "main");
    CHECK(metadata.model() == "main/test-model");
    CHECK(metadata.permission_mode() == "normal");
    CHECK(metadata.context_window_tokens() == 48000);
    // A NEW session is unnamed here: the free default name is minted by the
    // store's append transaction, not by creation. The name arrives as its own
    // metadata event after the first turn (tested below).

    // One emission per attach: attaching again is refused anyway, but the
    // second event would double the label refresh work. Assert the count is
    // exactly one by sending a turn and reading the terminator.
    REQUIRE(stream->Write(ChatTurn(session_id, "name me")));
    bool saw_turn_completed = false;
    std::optional<v1::SessionMetadataEvent> name_event;
    v1::ServerEvent next;
    while (stream->Read(&next)) {
        if (next.has_session_metadata()) {
            // A metadata event carrying the free default name. It must ride
            // BEFORE the turn terminator: TurnCompletedEvent is the
            // end-of-turn signal and a client may half-close on it -- anything
            // pushed after it wedges the client's drain-before-Finish.
            name_event = next.session_metadata();
            CHECK_FALSE(saw_turn_completed);
        }
        if (next.has_turn_completed()) {
            saw_turn_completed = true;
            break;
        }
    }
    REQUIRE(saw_turn_completed);
    REQUIRE(name_event.has_value());
    CHECK(name_event->name() == "name me");

    stream->WritesDone();
    v1::ServerEvent rest;
    while (stream->Read(&rest)) {
    }
    (void)stream->Finish();
}

TEST_CASE("the generic plugin channel dispatches PluginCall to the named endpoint",
          "[session_service][e5]") {
    ServiceFixture fixture;
    fixture.LoadEndpointPlugin();
    fixture.Start();

    // The client half: SessionClient's PluginCall wrapper, driven through a
    // real channel -- the exact call a client plugin's ClientApi makes.
    minimal::client::SessionClient client(rpc::SocketTarget(fixture.socket));

    const auto ping = client.PluginCall("fixture-endpoint", "ping", "1");
    REQUIRE(ping.status.ok());
    CHECK(ping.json_payload == "{\"pong\":1}");

    // Payloads are plugin-owned JSON; core transports, never interprets.
    const auto echo = client.PluginCall("fixture-endpoint", "echo.hello", "x");
    REQUIRE(echo.status.ok());
    CHECK(echo.json_payload == "x/hello");

    // An unknown pair is NOT_FOUND, not an empty answer.
    const auto ghost = client.PluginCall("no-such-plugin", "ping", "1");
    CHECK(ghost.status.error_code() == grpc::StatusCode::NOT_FOUND);

    // An endpoint THROW is contained: INTERNAL naming the failure, and the
    // service lives on (the next call answers normally).
    const auto boom = client.PluginCall("fixture-endpoint", "boom", "");
    CHECK(boom.status.error_code() == grpc::StatusCode::INTERNAL);
    const auto after = client.PluginCall("fixture-endpoint", "ping", "2");
    REQUIRE(after.status.ok());
    CHECK(after.json_payload == "{\"pong\":2}");
}

TEST_CASE("the generic plugin channel streams PluginSubscribe payloads",
          "[session_service][e5]") {
    ServiceFixture fixture;
    fixture.LoadEndpointPlugin();
    fixture.Start();

    minimal::client::SessionClient client(rpc::SocketTarget(fixture.socket));

    // The endpoint pushes ticks from its own thread; the wrapper delivers
    // them in order on the calling thread. The SUBSCRIBER owns the lifetime:
    // returning false cancels the call (TryCancel) -- here after the first
    // three payloads.
    std::vector<std::string> seen;
    const auto outcome = client.PluginSubscribe(
        "fixture-endpoint", "ticks", "99",
        [&](const std::string& payload) {
            seen.push_back(payload);
            return seen.size() < 3;
        });
    CHECK(outcome == minimal::client::SubscribeOutcome::kCancelled);
    REQUIRE(seen.size() == 3);
    CHECK(seen[0] == "tick-0");
    CHECK(seen[1] == "tick-1");
    CHECK(seen[2] == "tick-2");

    // An unknown method is refused before any payload: kError, not a silent
    // false that a subscriber could misread as its own cancellation.
    grpc::Status refused_status;
    CHECK(client.PluginSubscribe("fixture-endpoint", "no-such-method", "", {},
                                 &refused_status) ==
          minimal::client::SubscribeOutcome::kError);
    CHECK_FALSE(refused_status.ok());
}

TEST_CASE("a PluginSubscribe call outliving SessionClient's destruction doesn't use a "
          "freed channel",
          "[session_service][e5]") {
    // A subscription is designed to run for a plugin's whole lifetime, on
    // the plugin's own thread, with no deadline -- ~SessionClient's
    // CancelSubscriptions only REQUESTS cancellation and returns
    // immediately; it does not wait for this call to actually finish.
    // Before the fix, PluginSubscribe used this->stub_/this->channel_, so a
    // subscriber still inside Read()/Finish() when SessionClient's members
    // were destroyed dereferenced freed memory through gRPC's completion
    // queue. Meaningful primarily under ASan.
    ServiceFixture fixture;
    fixture.LoadEndpointPlugin();
    fixture.Start();

    auto client =
        std::make_unique<minimal::client::SessionClient>(rpc::SocketTarget(fixture.socket));

    std::atomic<int> seen{0};
    std::atomic<bool> subscriber_done{false};
    std::thread subscriber([&] {
        client->PluginSubscribe("fixture-endpoint", "ticks", "10000",
                                [&](const std::string&) {
                                    ++seen;
                                    return true;   // never stops on its own
                                });
        subscriber_done = true;
    });

    // Let the subscription actually start receiving before destroying the
    // client out from under it.
    while (seen.load() == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));

    client.reset();   // ~SessionClient: CancelSubscriptions(), then destroys channel_/stub_
    // The subscriber's OWN local channel/stub copy (the fix) keeps it alive
    // independent of `client` -- it must finish cleanly rather than hang or
    // crash.
    subscriber.join();
    CHECK(subscriber_done.load());
}

TEST_CASE("PluginSubscribe after CancelSubscriptions is refused immediately, not left "
          "uncancelled",
          "[session_service][e5]") {
    // CancelSubscriptions used to be a one-time sweep, not a latch: a
    // subscribe call landing AFTER it ran got a fresh, uncancelled context
    // with no deadline -- nothing would ever cancel it, hanging whoever
    // joins that call's thread at quit (a plugin's own retry-on-error is
    // the realistic trigger).
    ServiceFixture fixture;
    fixture.LoadEndpointPlugin();
    fixture.Start();

    // shared_ptr, not a local: a hang is the failure mode under test, so
    // this runs on a DETACHED thread (deadline.hpp) that must own every
    // object it touches -- a reference into this test's stack would crash
    // on top of the failure if the wait below ever times out.
    auto client =
        std::make_shared<minimal::client::SessionClient>(rpc::SocketTarget(fixture.socket));
    client->CancelSubscriptions();

    auto result = minimal::testing::RunInBackground([client] {
        return client->PluginSubscribe("fixture-endpoint", "ticks", "10000",
                                       [](const std::string&) { return true; });
    });
    REQUIRE(result.WaitFor(std::chrono::seconds(2)));
    CHECK(result.Get() == minimal::client::SubscribeOutcome::kCancelled);
}

TEST_CASE("ListModels lists static refs cold and discovered refs after refresh",
          "[session_service][models]") {
    ServiceFixture fixture;
    // A discover-entry the config never names models for.
    fixture.Write("providers.json",
                  R"({"providers":{"main":{"backend":"fake","url":"http://localhost/v1",
                      "models":{"test-model":{}}},
                     "disc":{"backend":"fake","url":"http://disc/v1","discover":true}}})");
    fixture.Start();
    fixture.catalog->SetDiscoveryHook([](const std::string&, const llm::ProviderConfig&) {
        return config::DiscoveryOutcome{
            /*catalog_present=*/true, /*ok=*/true, {},
            {llm::DiscoveredModel{"discovered-model", 12345, nlohmann::json::object(),
                                  std::nullopt}}};
    });

    // Cold, the discover-entry contributes nothing (a listing never triggers
    // the lazy fetch).
    {
        minimal::client::SessionClient client(rpc::SocketTarget(fixture.socket));
        const auto result = client.ListModels({});
        REQUIRE(result.status.ok());
        REQUIRE(result.response.models().size() == 1);
        CHECK(result.response.models(0).ref() == "main/test-model");
    }

    // refresh_discovery fetches NOW; the discovered model joins the listing
    // with its context window, and a later bare listing serves it from cache.
    minimal::client::SessionClient client(rpc::SocketTarget(fixture.socket));
    {
        minimal::client::SessionClient::ListModelsOptions opts;
        opts.refresh_discovery = true;
        const auto result = client.ListModels(opts);
        REQUIRE(result.status.ok());
        REQUIRE(result.response.models().size() == 2);
        // Refs are sorted ascending: "disc/..." before "main/...".
        CHECK(result.response.models(0).ref() == "disc/discovered-model");
        CHECK(result.response.models(0).context_window_tokens() == 12345);
        CHECK(result.response.models(1).ref() == "main/test-model");
    }
    {
        const auto warm = client.ListModels({});
        REQUIRE(warm.status.ok());
        CHECK(warm.response.models().size() == 2);
    }
}

TEST_CASE("a second client on the same session is served the same events",
          "[session_service][invariant]") {
    // S5 FLIPS THE OLD INVARIANT. The outbound side is a broadcaster with one
    // drop-oldest queue per attached stream (event_broadcaster.hpp), so a
    // second stream is SERVED, not refused -- that is the seam multi-client
    // attach (a web UI beside the TUI) rides on. The old single-consumer
    // design would have split the event stream between two writers draining
    // one queue; Deliver copies per subscriber instead, so both streams see
    // the same payloads in the same order.
    ServiceFixture fixture;
    fixture.Script(testing::TextResponse("hi"));
    fixture.Script(testing::TextResponse("and again"));
    fixture.Start();

    const std::string session_id = fixture.CreateSession();

    // Attach #1 must COMPLETE before #2 is even written. The broadcaster's
    // Subscribe() decides resync-vs-direct-metadata by ever_subscribed_
    // (event_broadcaster.cpp), and gRPC orders two independent streams'
    // handlers neither way -- without this read, the second stream could win
    // the race, attach first, and take the first-subscriber path: a
    // wrong-order test failure, not a server one (observed ~1 run in 8).
    // The metadata arriving proves the subscription is registered.
    grpc::ClientContext first_context;
    auto first = fixture.stub->OpenSession(&first_context);
    v1::ClientEvent naming;
    naming.set_session_id(session_id);
    naming.mutable_attach();
    v1::ServerEvent event;
    REQUIRE(first->Write(naming));
    REQUIRE(first->Read(&event));
    REQUIRE(event.has_session_metadata());

    grpc::ClientContext second_context;
    auto second = fixture.stub->OpenSession(&second_context);
    REQUIRE(second->Write(naming));

    // The attach-time delivery notices: the first subscriber took its
    // metadata above; the LATE one is told to resync first (it missed
    // everything the first subscriber's stream took -- at minimum the
    // transcript) and gets its metadata after.
    REQUIRE(second->Read(&event));
    REQUIRE(event.has_resync_required());
    REQUIRE(event.resync_required().reason() == "late_attach");
    REQUIRE(second->Read(&event));
    REQUIRE(event.has_session_metadata());

    // A turn submitted on the FIRST stream is visible on BOTH, in the same
    // payload order, ending in turn_completed on each.
    REQUIRE(first->Write(ChatTurn(session_id, "hello")));
    const auto first_payloads = ReadUntilTurnCompleted(*first);
    const auto second_payloads = ReadUntilTurnCompleted(*second);
    REQUIRE_FALSE(first_payloads.empty());
    REQUIRE(first_payloads == second_payloads);
    CHECK(first_payloads.back() == v1::ServerEvent::kTurnCompleted);

    // Either stream can keep driving the session.
    REQUIRE(second->Write(ChatTurn(session_id, "and again?")));
    CHECK(WaitForTurnCompleted(*second));
    CHECK(WaitForTurnCompleted(*first));


    (void)DrainAndFinish(first);
    (void)DrainAndFinish(second);
}

TEST_CASE("a subscriber that stops reading cannot stall the session's turn",
          "[session_service][invariant]") {
    // THE INVARIANT (S5): the turn is never gated on any subscriber. A wedged
    // stream's queue drops its own oldest once full and latches a resync
    // notice; the live subscriber keeps receiving everything in order. The
    // old single-queue design ran the whole turn on the reader's backpressure:
    // one stalled client parked the worker.
    ServiceFixture fixture;
    fixture.Script(testing::TextResponse("hi"), {llm::StreamEvent{
                                                     llm::TextDeltaEvent{0, "chatty"}}});
    fixture.Start();

    const std::string session_id = fixture.CreateSession();

    grpc::ClientContext wedged_context;
    auto wedged = fixture.stub->OpenSession(&wedged_context);
    v1::ClientEvent naming;
    naming.set_session_id(session_id);
    naming.mutable_attach();
    REQUIRE(wedged->Write(naming));

    // Attach #1 must COMPLETE before the live stream attaches. Subscribe()
    // decides resync-vs-direct-metadata by ever_subscribed_
    // (event_broadcaster.cpp), and gRPC orders two independent streams'
    // handlers neither way -- without this read the live stream could win
    // the race, attach first, and take the first-subscriber path (metadata
    // with no resync notice): a wrong-order test failure, not a server one
    // (observed ~1 run in 8). The wedged stream reads its attach metadata,
    // then goes silent -- everything after sits unread, which is the point.
    v1::ServerEvent event;
    REQUIRE(wedged->Read(&event));
    REQUIRE(event.has_session_metadata());

    grpc::ClientContext live_context;
    auto live = fixture.stub->OpenSession(&live_context);
    REQUIRE(live->Write(naming));

    // The live stream attached deterministically SECOND: it is told to
    // resync (it missed the wedged stream's window) before its own metadata.
    REQUIRE(live->Read(&event));
    REQUIRE(event.has_resync_required());
    REQUIRE(live->Read(&event));
    REQUIRE(event.has_session_metadata());

    REQUIRE(live->Write(ChatTurn(session_id, "hello")));
    // The live stream sees the whole turn -- the wedged one stopped reading
    // after its attach metadata, so this proves delivery does not wait on it.
    CHECK(WaitForTurnCompleted(*live));

    // The session survived the wedged reader.
    CHECK(fixture.sessions->Find(session_id) != nullptr);
    (void)DrainAndFinish(live);
    (void)DrainAndFinish(wedged);
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

namespace {

// A read-only command that answers only when released -- the flood test parks
// the whole command pool on it, which is the state in which saturation (and the
// refusal answer it owes each client) becomes observable.
class GatedReadOnlyCommand : public commands::HarnessCommand {
public:
    explicit GatedReadOnlyCommand(std::shared_ptr<std::atomic<bool>> release)
        : release_(std::move(release)) {}

    std::string_view Name() const override { return "gate"; }
    std::string_view Summary() const override { return "waits to be released"; }
    std::string_view Usage() const override { return "/gate"; }
    bool ReadOnly() const override { return true; }

    commands::CommandResult Execute(commands::CommandContext&,
                                     const commands::CommandArgs&) override {
        while (!release_->load(std::memory_order_acquire)) std::this_thread::sleep_for(1ms);
        return commands::CommandResult::Ok("gated");
    }

private:
    std::shared_ptr<std::atomic<bool>> release_;
};

}  // namespace

TEST_CASE("a flood of read-only commands is bounded and every request is answered",
          "[session_service][commands]") {
    // The command pool replaced detached per-command threads, which made a
    // flood of read-only commands an unbounded pile of never-joined threads and
    // left a shutdown window where a still-running one held shared_ptrs into
    // registries being destroyed. Bounded means saturated posts are REFUSED --
    // and a refusal still owes each request_id an answer, never silence. If
    // this test hangs, a command task is being lost or leaked.
    ServiceFixture fixture;
    auto gate = std::make_shared<std::atomic<bool>>(false);
    fixture.commands.Register(std::make_unique<GatedReadOnlyCommand>(gate));
    fixture.Start();

    const std::string session_id = fixture.CreateSession();
    grpc::ClientContext context;
    auto stream = fixture.stub->OpenSession(&context);
    v1::ClientEvent attach;
    attach.set_session_id(session_id);
    attach.mutable_attach();  // name the stream without submitting work
    REQUIRE(stream->Write(attach));

    constexpr int kCommands = 200;
    for (int i = 0; i < kCommands; ++i) {
        v1::ClientEvent invocation;
        invocation.set_session_id(session_id);
        auto* command = invocation.mutable_command();
        command->set_request_id("cmd-" + std::to_string(i));
        command->set_name("gate");
        REQUIRE(stream->Write(invocation));
    }

    // Collect answers on a reader thread: the pool's accepted-but-parked tasks
    // cannot answer until the gate opens, so a single-threaded Read() here
    // would block waiting for events that need the release below first.
    std::mutex mu;
    std::map<std::string, int> answers;  // request_id -> how many times answered
    auto reader = testing::RunInBackground([&] {
        v1::ServerEvent event;
        while (stream->Read(&event)) {
            if (event.has_command_result()) {
                std::lock_guard<std::mutex> lock(mu);
                ++answers[event.command_result().request_id()];
                if (static_cast<int>(answers.size()) >= kCommands) return true;
            }
        }
        return true;
    });

    std::this_thread::sleep_for(100ms);  // the pool has now saturated or parked
    gate->store(true, std::memory_order_release);
    REQUIRE(reader.WaitFor(10s));

    {
        std::lock_guard<std::mutex> lock(mu);
        REQUIRE(static_cast<int>(answers.size()) == kCommands);
        for (const auto& [request_id, count] : answers) {
            CHECK(count == 1);  // exactly one answer per request, ok or error
        }
    }
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
        // S5.2: a bare drop only detaches now, so an explicit TERMINATE is
        // what actually ends the session -- this test wants it gone, for the
        // resume-from-store path below.
        REQUIRE(stream->Write(EndSession(session_id, v1::EndSession::TERMINATE)));
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
    // new turn -- asserted while the session is still alive (the Session holds
    // its provider). Removing the seeded-history load fails this: the provider
    // would see only the new user message. Requests are recorded in the SHARED
    // script: request 1 is phase 1's turn, request 2 is the resumed one.
    REQUIRE(fixture.script->requests.size() == 2);
    const auto& sent = fixture.script->requests.back().messages;
    REQUIRE(sent.size() == 3);
    CHECK(std::get<llm::TextBlock>(sent[0].content.front()).text == "remember the number 7");
    CHECK(std::get<llm::TextBlock>(sent[1].content.front()).text == "first answer");
    CHECK(std::get<llm::TextBlock>(sent[2].content.front()).text == "what number did I give you?");

    REQUIRE(stream->Write(EndSession(session_id, v1::EndSession::TERMINATE)));
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
    fixture.Script(testing::TextResponse("and again"));
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
    REQUIRE(fixture.script->requests.size() == 1);
    const auto& sent = fixture.script->requests.front().messages;
    REQUIRE(sent.size() == 4);
    REQUIRE(sent[2].content.size() == 1);
    const auto* repair = std::get_if<llm::ToolResultBlock>(&sent[2].content.front());
    REQUIRE(repair != nullptr);
    CHECK(repair->tool_use_id == "call-1");
    CHECK(repair->is_error);

    REQUIRE(stream->Write(EndSession(row.session_id, v1::EndSession::TERMINATE)));
    (void)DrainAndFinish(stream);
    WaitForTerminated(*fixture.sessions, row.session_id);
}

TEST_CASE("a failed resume leaves the stored session intact", "[session_service][resume]") {
    // Unlike fresh creation -- which rolls back its just-minted row when the
    // Session fails to construct -- a failed resume must never touch the row:
    // it is the user's existing session, not one this call made.
    ServiceFixture fixture;
    fixture.Script(testing::TextResponse("hi"));
    fixture.Script(testing::TextResponse("and again"));
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

TEST_CASE("a store fault on resume is never reported as a missing session",
          "[session_service][resume][robustness]") {
    // GetSession used to answer nullopt for SQLITE_BUSY/IOERR/CORRUPT exactly
    // as for an absent row, so a transient store fault told the client "no such
    // session" -- the one answer it cannot recover from -- and the handler's
    // unchecked re-read dereference was UB. The store now throws on failure and
    // the handler maps it to INTERNAL. Corruption meeting the query or not is
    // nondeterministic (same harness as the store-level tests); the invariant
    // is one-directional and deterministic: a session that EXISTS is never
    // answered NOT_FOUND, and the handler never crashes.
    ServiceFixture fixture;
    fixture.Script(testing::TextResponse("first answer"));
    fixture.Start();

    const std::string session_id = fixture.CreateSession();
    {
        grpc::ClientContext context;
        auto stream = fixture.stub->OpenSession(&context);
        REQUIRE(stream->Write(ChatTurn(session_id, "hello")));
        REQUIRE(WaitForTurnCompleted(*stream));
        REQUIRE(stream->Write(EndSession(session_id, v1::EndSession::TERMINATE)));
        (void)DrainAndFinish(stream);
    }
    WaitForTerminated(*fixture.sessions, session_id);

    // Scribble the database files the service's connection has open. In WAL
    // mode the freshly-committed rows live in the -wal file until a checkpoint,
    // so both are scribbled: whichever read meets the damage fails loudly
    // (SQLITE_CORRUPT/IOERR) and must surface as INTERNAL -- never as
    // NOT_FOUND for a session that exists.
    for (const auto* suffix : {"", "-wal"}) {
        std::error_code ec;
        const auto db_path =
            std::filesystem::path(fixture.dir / "sessions.db").string() + suffix;
        const auto size = std::filesystem::file_size(db_path, ec);
        if (ec || size < 2048) continue;
        std::fstream file(db_path, std::ios::in | std::ios::out | std::ios::binary);
        REQUIRE(file);
        file.seekp(static_cast<std::streamoff>(size) / 2);
        const std::string garbage(static_cast<std::size_t>(size) / 4, '\xa5');
        file.write(garbage.data(), static_cast<std::streamsize>(garbage.size()));
    }

    v1::CreateSessionResponse resumed;
    const auto status = ResumeOverWire(*fixture.stub, session_id, &resumed);
    CHECK(status.error_code() != grpc::StatusCode::NOT_FOUND);
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
        REQUIRE(stream->Write(EndSession(session_id, v1::EndSession::TERMINATE)));
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

TEST_CASE("a resumed session restores the model persisted with it",
          "[session_service][resume][invariant]") {
    // THE RESTART BUG: the model override used to live only in server memory,
    // so create-with---model, close, restart, resume re-resolved the bare
    // preset and its default model won -- the user's choice silently undone.
    // The choice now rides the session row, and a model-LESS resume restores
    // it. (The TUI's picker resume sends no model; this is its exact shape.)
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

    // Created WITH --model, one turn, then terminated -- the transcript and
    // the model choice both survive in SQLite.
    grpc::ClientContext create_context;
    v1::CreateSessionRequest create_request;
    create_request.set_project_root((fixture.dir / "work").string());
    create_request.set_agent_preset("main");
    create_request.set_model("main/other-model");
    v1::CreateSessionResponse created;
    REQUIRE(fixture.stub->CreateSession(&create_context, create_request, &created).ok());
    const std::string session_id = created.session().session_id();
    // The row already carries the choice at creation time.
    REQUIRE(fixture.db->GetSession(session_id).has_value());
    CHECK(*fixture.db->GetSession(session_id)->model == "main/other-model");
    {
        grpc::ClientContext context;
        auto stream = fixture.stub->OpenSession(&context);
        REQUIRE(stream->Write(ChatTurn(session_id, "hello")));
        REQUIRE(WaitForTurnCompleted(*stream));
        REQUIRE(stream->Write(EndSession(session_id, v1::EndSession::TERMINATE)));
        (void)DrainAndFinish(stream);
    }
    WaitForTerminated(*fixture.sessions, session_id);

    SECTION("a model-less resume restores the stored choice") {
        v1::CreateSessionResponse resumed;
        REQUIRE(ResumeOverWire(*fixture.stub, session_id, &resumed).ok());

        auto session = fixture.sessions->Find(session_id);
        REQUIRE(session != nullptr);
        CHECK(session->preset().model_ref.str() == "main/other-model");
        REQUIRE(session->preset().max_tokens.has_value());
        CHECK(*session->preset().max_tokens == 512);
    }

    SECTION("a resume with its own model rewrites the stored choice") {
        // "Continue this conversation, but on the other model": the request
        // outranks the row, and the row is brought up to date -- the NEXT
        // model-less resume keeps THIS choice, not the superseded one.
        v1::CreateSessionResponse resumed;
        REQUIRE(ResumeOverWire(*fixture.stub, session_id, &resumed, "main/test-model").ok());

        auto session = fixture.sessions->Find(session_id);
        REQUIRE(session != nullptr);
        CHECK(session->preset().model_ref.str() == "main/test-model");
        CHECK(*fixture.db->GetSession(session_id)->model == "main/test-model");
    }
}

TEST_CASE("a resumed session restores the subagent-model persisted with it",
          "[session_service][resume][invariant]") {
    // /subagent-model's restart story, same as the model override above: the
    // choice is session state, so a restart must not silently revert spawned
    // children to inheriting the parent's model.
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
    for (auto& command : commands_core::MakeCoreCommands()) {
        fixture.commands.Register(std::move(command));
    }
    fixture.Start();

    const std::string session_id = fixture.CreateSession();
    REQUIRE_FALSE(session_id.empty());

    {
        grpc::ClientContext context;
        auto stream = fixture.stub->OpenSession(&context);
        const auto set = RunCommand(stream, session_id, "subagent-model", {"main/other-model"});
        REQUIRE(set.has_value());
        CHECK(set->status() == v1::COMMAND_STATUS_OK);
        // The row already carries the choice.
        REQUIRE(fixture.db->GetSession(session_id).has_value());
        REQUIRE(fixture.db->GetSession(session_id)->subagent_model.has_value());
        CHECK(*fixture.db->GetSession(session_id)->subagent_model == "main/other-model");

        REQUIRE(stream->Write(EndSession(session_id, v1::EndSession::TERMINATE)));
        (void)DrainAndFinish(stream);
    }
    WaitForTerminated(*fixture.sessions, session_id);

    v1::CreateSessionResponse resumed;
    REQUIRE(ResumeOverWire(*fixture.stub, session_id, &resumed).ok());

    auto session = fixture.sessions->Find(session_id);
    REQUIRE(session != nullptr);
    REQUIRE(session->subagent_model_override().has_value());
    CHECK(session->subagent_model_override()->str() == "main/other-model");
}

TEST_CASE("a client-supplied page limit is clamped, not trusted",
          "[session_service][scrollback]") {
    // `limit` is a wire int32 with a floor and, until this, no ceiling.
    // SessionDatabase binds `limit + 1` for the has-more probe, so INT32_MAX is
    // signed overflow -- and in practice binds a NEGATIVE limit, which SQLite
    // reads as no limit at all. One request then loads the whole transcript:
    // every row decoded, every message copied into one proto response. That is
    // the O(n) blowup the keyset paging exists to prevent, and it is reachable
    // from any client, since authentication is pass-through by default.
    ServiceFixture fixture;
    fixture.Start();

    const std::string session_id = fixture.CreateSession();
    // More rows than the ceiling, or the assertion below passes on a short
    // transcript whether or not anything clamps.
    for (int i = 0; i < 1010; ++i) {
        fixture.db->AppendMessage(
            session_id, llm::Message{llm::Role::User, {llm::TextBlock{"m" + std::to_string(i)}}}, 0);
    }

    grpc::ClientContext context;
    v1::GetMessagesRequest request;
    request.set_session_id(session_id);
    request.set_limit(std::numeric_limits<std::int32_t>::max());
    v1::GetMessagesResponse response;

    // Answered normally rather than refused: asking for more than we serve is
    // not an error, and has_more already tells the client to page.
    REQUIRE(fixture.stub->GetMessages(&context, request, &response).ok());
    CHECK(response.messages_size() == 1000);
    CHECK(response.has_more());

    grpc::ClientContext list_context;
    v1::ListSessionsRequest list;
    list.set_include_all_dirs(true);
    list.set_limit(std::numeric_limits<std::int32_t>::max());
    v1::ListSessionsResponse listed;
    REQUIRE(fixture.stub->ListSessions(&list_context, list, &listed).ok());
    CHECK(listed.sessions_size() <= 1000);  // one session here; the clamp is the same call
}

TEST_CASE("an LLM command is refused mid-turn while a mutating one still queues",
          "[session_service][commands][invariant]") {
    // THE GUARD THAT NOTHING EVER TRIGGERED. Dispatch() takes a `busy` flag
    // whose whole purpose is this refusal, and both production callers passed
    // `false` unconditionally -- so only the unit tests exercised it, and
    // /compact typed during a long turn was quietly queued instead. That is
    // verbatim what command_dispatcher.hpp says must not happen: "queueing
    // would make /compact silently run minutes later against a context that had
    // moved on."
    //
    // The pair is the point. An LLMCommand is refused; a mutating
    // HarnessCommand still QUEUES and lands at the turn boundary, because the
    // shared work queue exists to serve exactly that. Refusing on !ReadOnly()
    // would have taken both.
    ServiceFixture fixture;
    fixture.commands.Register(std::make_unique<testing::RecordingLlmCommand>());
    fixture.commands.Register(std::make_unique<testing::RecordingCommand>());
    fixture.tools.Register(
        std::make_unique<testing::FakeTool>("read", tools::ToolCategory::ReadOnly, "contents"));
    fixture.Script(testing::ToolUseResponse("call-1", "read", {{"path", "a.txt"}}));
    fixture.Script(testing::TextResponse("done"));
    fixture.Start();

    const std::string session_id = fixture.CreateSession();
    grpc::ClientContext context;
    auto stream = fixture.stub->OpenSession(&context);

    // All three written before any read, so both commands are dispatched while
    // the turn is still queued or running -- idle() is false the moment
    // SubmitTurn lands, and the reader dispatches these in order.
    REQUIRE(stream->Write(ChatTurn(session_id, "read it")));
    v1::ClientEvent llm_invocation;
    llm_invocation.set_session_id(session_id);
    llm_invocation.mutable_command()->set_request_id("cmd-llm");
    llm_invocation.mutable_command()->set_name("llmrecord");
    REQUIRE(stream->Write(llm_invocation));
    v1::ClientEvent harness_invocation;
    harness_invocation.set_session_id(session_id);
    harness_invocation.mutable_command()->set_request_id("cmd-harness");
    harness_invocation.mutable_command()->set_name("record");
    REQUIRE(stream->Write(harness_invocation));

    std::optional<v1::CommandResult> llm_result;
    std::optional<v1::CommandResult> harness_result;
    v1::ServerEvent event;
    while (stream->Read(&event)) {
        if (!event.has_command_result()) continue;
        if (event.command_result().request_id() == "cmd-llm") llm_result = event.command_result();
        if (event.command_result().request_id() == "cmd-harness") {
            harness_result = event.command_result();
        }
        if (llm_result && harness_result) break;
    }
    (void)DrainAndFinish(stream);

    REQUIRE(llm_result.has_value());
    CHECK(llm_result->status() == v1::COMMAND_STATUS_ERROR);
    CHECK(llm_result->message().find("in flight") != std::string::npos);

    REQUIRE(harness_result.has_value());
    CHECK(harness_result->status() == v1::COMMAND_STATUS_OK);
}

TEST_CASE("an unknown command is answered immediately, not after the turn",
          "[session_service][commands]") {
    // It used to be queued behind the running turn purely to be told "unknown
    // command" minutes later. Nothing about that reply depends on the worker.
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
    REQUIRE(stream->Write(ChatTurn(session_id, "read it")));
    v1::ClientEvent invocation;
    invocation.set_session_id(session_id);
    invocation.mutable_command()->set_request_id("cmd-x");
    invocation.mutable_command()->set_name("no-such-command");
    REQUIRE(stream->Write(invocation));

    // Arrives BEFORE turn_completed: the answer overtakes the turn it was not
    // waiting on.
    bool saw_turn_completed = false;
    std::optional<v1::CommandResult> result;
    v1::ServerEvent event;
    while (stream->Read(&event)) {
        if (event.has_turn_completed()) saw_turn_completed = true;
        if (event.has_command_result()) {
            result = event.command_result();
            break;
        }
    }
    (void)DrainAndFinish(stream);

    REQUIRE(result.has_value());
    CHECK(result->status() == v1::COMMAND_STATUS_ERROR);
    CHECK(result->message().find("unknown command") != std::string::npos);
    CHECK_FALSE(saw_turn_completed);
}

TEST_CASE("observers see every tool call and the turn's tool list",
          "[session_service][observer][invariant]") {
    // BOTH HALVES OF THE CONTRACT WERE DECLARED AND NEITHER WAS PRODUCED.
    // ARCHITECTURE.md documents OnToolExecuted as one of the four SessionObserver
    // hooks and TurnSummary::tool_names_invoked as part of the payload; track I
    // is checked off. But NotifyToolExecuted's only caller in the entire tree was
    // its own unit test, and RunOneTurn built TurnSummary without ever setting
    // tool_names_invoked. A plugin author implementing the documented interface
    // got silence and an empty vector -- worse than the hook not existing,
    // because it reads as working.
    struct Recorder : agent::SessionObserver {
        std::mutex mu;
        std::vector<std::string> executed;
        std::vector<std::string> summarized;
        std::string permission_key;
        std::string category;
        void OnToolExecuted(const agent::SessionInfo&,
                            const agent::ToolInvocation& invocation) override {
            std::lock_guard<std::mutex> lock(mu);
            executed.push_back(invocation.tool_name);
            permission_key = invocation.permission_key;
            category = invocation.category;
        }
        void OnTurnCompleted(const agent::SessionInfo&,
                             const agent::TurnSummary& summary) override {
            std::lock_guard<std::mutex> lock(mu);
            summarized = summary.tool_names_invoked;
        }
    };

    ServiceFixture fixture;
    auto recorder = std::make_shared<Recorder>();
    fixture.tools.Register(
        std::make_unique<testing::FakeTool>("read", tools::ToolCategory::ReadOnly, "contents"));
    fixture.Script(testing::ToolUseResponse("call-1", "read", {{"path", "a.txt"}}));
    fixture.Script(testing::TextResponse("done"));
    fixture.Start();
    // After Start(): the fixture builds the observer registry there, and every
    // session is created later still, so nothing is missed.
    fixture.observers->Register(recorder);

    const std::string session_id = fixture.CreateSession();
    grpc::ClientContext context;
    auto stream = fixture.stub->OpenSession(&context);
    REQUIRE(stream->Write(ChatTurn(session_id, "read it")));
    REQUIRE(WaitForTurnCompleted(*stream));
    (void)DrainAndFinish(stream);
    fixture.observers->WaitForIdleForTesting();

    std::lock_guard<std::mutex> lock(recorder->mu);
    CHECK((recorder->executed == std::vector<std::string>{"read"}));
    CHECK((recorder->summarized == std::vector<std::string>{"read"}));
    // The labels core.post_tool_use filters on (hook_observer_bridge): they
    // were registered as filterable but never produced.
    CHECK(recorder->permission_key == "read");
    CHECK(recorder->category == "read_only");
}

TEST_CASE("an advisor answers Ask; abstention still reaches the human over the wire",
          "[session_service][advisor]") {
    // The full advisor path over a real socket, through the real plugin loader:
    // auto_tool is allowed by the fixture advisor without a prompt; manual_tool
    // is abstained on, so the wire prompt arrives and a human answer -- the
    // AllowOnly-forbidden path -- completes the turn.
    ServiceFixture fixture;
    fixture.service_permissions.advisor = "fixture-advisor";
    fixture.tools.Register(std::make_unique<testing::FakeTool>("auto_tool",
                                                               tools::ToolCategory::Execute,
                                                               "auto ran"));
    fixture.tools.Register(std::make_unique<testing::FakeTool>("manual_tool",
                                                               tools::ToolCategory::Execute,
                                                               "manual ran"));
    fixture.LoadAdvisorPlugin();
    fixture.Script(testing::ToolUseResponse("call-1", "auto_tool", nlohmann::json::object()));
    fixture.Script(testing::TextResponse("auto done"));
    fixture.Script(testing::ToolUseResponse("call-2", "manual_tool", nlohmann::json::object()));
    fixture.Script(testing::TextResponse("manual done"));
    fixture.Write("permissions.json", nlohmann::json{{"fallback", {{"execute", "ask"}}}}.dump());
    fixture.Start();

    const std::string session_id = fixture.CreateSession();
    grpc::ClientContext context;
    auto stream = fixture.stub->OpenSession(&context);
    REQUIRE(stream->Write(ChatTurn(session_id, "run auto")));

    v1::ServerEvent event;
    bool prompted = false;
    bool answered = false;
    int tool_finishes = 0;
    int turn_completions = 0;
    bool second_turn_sent = false;
    while (stream->Read(&event)) {
        if (event.has_permission_request()) {
            prompted = true;
            // The advisor allowed auto_tool without a prompt; the ONLY prompt on
            // this stream is the abstained manual_tool.
            CHECK(event.permission_request().tool_name() == "manual_tool");
            v1::ClientEvent answer;
            answer.set_session_id(session_id);
            auto* response = answer.mutable_permission_response();
            response->set_request_id(event.permission_request().request_id());
            response->set_decision(v1::PERMISSION_DECISION_ALLOW);
            REQUIRE(stream->Write(answer));
            answered = true;
        }
        if (event.has_tool_execution() && !event.tool_execution().started()) ++tool_finishes;
        if (event.has_turn_completed()) {
            ++turn_completions;
            if (turn_completions == 1) {
                // The stream stays open across the terminator; turn 2 exercises
                // the abstention path.
                REQUIRE(stream->Write(ChatTurn(session_id, "run manual")));
                second_turn_sent = true;
            }
        }
        if (event.has_error()) break;
        if (turn_completions == 2) break;
    }
    (void)DrainAndFinish(stream);

    CHECK(answered);
    CHECK(prompted);
    CHECK(second_turn_sent);
    CHECK(tool_finishes == 2);        // both tools ran
    CHECK(turn_completions == 2);     // and the turn terminator closed each turn
}

// --- Session traversal (rewind) over the real channel ------------------------

// The typesafe-advisor E2E: the SHIPPED advisor -- its own sources behind a
// fixture .so, registered through the real loader with a config slice, named
// by permissions.json -- resolves an Ask over a real socket, against a loopback
// stub standing in for the System One endpoint. This is the full length of the
// track-J path with the production plugin at the end of it: the same registration
// attribution permissions.json selects on, the same composition in
// FinishSessionWiring, the same thresholds, and the same grant behavior.
TEST_CASE("the typesafe-advisor answers a confident Ask and abstains below the margin",
          "[session_service][advisor][typesafe]") {
    ServiceFixture fixture;
    fixture.tools.Register(std::make_unique<testing::FakeTool>("auto_tool",
                                                               tools::ToolCategory::Execute,
                                                               "auto ran"));
    fixture.tools.Register(std::make_unique<testing::FakeTool>("manual_tool",
                                                               tools::ToolCategory::Execute,
                                                               "manual ran"));

    // Two responses consumed in arrival order across the two tool calls:
    // call 1 (auto_tool) -> confident allow (live-fixture shape, 0.91/0.09,
    // both gates clear); call 2 (manual_tool) -> confident REVIEW (0.86, the
    // captured abstain path).
    JevStubServer stub({JevFixtureBody("allow-response.json"),
                        JevFixtureBody("review-response.json")});

    // Load the REAL advisor through the REAL loader, with the stub URL riding
    // the config slice -- a plugins.json entry in miniature.
    fixture.LoadAdvisorPlugin(nlohmann::json{{"endpoint", stub.url()}, {"api_key", "test-key"}},
                              "typesafe-advisor-fixture.so");
    fixture.service_permissions.advisor = "typesafe-advisor-fixture";
    fixture.Script(testing::ToolUseResponse("call-1", "auto_tool", nlohmann::json::object()));
    fixture.Script(testing::TextResponse("auto done"));
    fixture.Script(testing::ToolUseResponse("call-2", "manual_tool", nlohmann::json::object()));
    fixture.Script(testing::TextResponse("manual done"));
    fixture.Write("permissions.json", nlohmann::json{{"fallback", {{"execute", "ask"}}}}.dump());
    fixture.Start();

    const std::string session_id = fixture.CreateSession();
    grpc::ClientContext context;
    auto stream = fixture.stub->OpenSession(&context);
    REQUIRE(stream->Write(ChatTurn(session_id, "run auto")));

    v1::ServerEvent event;
    bool prompted = false;
    bool answered = false;
    int tool_finishes = 0;
    int turn_completions = 0;
    bool second_turn_sent = false;
    while (stream->Read(&event)) {
        if (event.has_permission_request()) {
            prompted = true;
            // The advisor auto-allowed auto_tool (the stub's first response);
            // the ONLY prompt on this stream is manual_tool -- its review
            // answer abstained and fell through to the wire checker, the
            // human, exactly as designed.
            CHECK(event.permission_request().tool_name() == "manual_tool");
            v1::ClientEvent answer;
            answer.set_session_id(session_id);
            auto* response = answer.mutable_permission_response();
            response->set_request_id(event.permission_request().request_id());
            response->set_decision(v1::PERMISSION_DECISION_ALLOW);
            REQUIRE(stream->Write(answer));
            answered = true;
        }
        if (event.has_tool_execution() && !event.tool_execution().started()) ++tool_finishes;
        if (event.has_turn_completed()) {
            ++turn_completions;
            if (turn_completions == 1) {
                REQUIRE(stream->Write(ChatTurn(session_id, "run manual")));
                second_turn_sent = true;
            }
        }
        if (event.has_error()) break;
        if (turn_completions == 2) break;
    }
    (void)DrainAndFinish(stream);

    CHECK(answered);
    CHECK(prompted);
    CHECK(second_turn_sent);
    CHECK(tool_finishes == 2);
    CHECK(turn_completions == 2);
}

// --- Session traversal (rewind) over the real channel (original) --------------

// Drives a full interactive session on ONE stream -- closing a stream
// terminates the session, so turn 1, the rewind, and the fork turn all share
// it, exactly as a tab would.
TEST_CASE("a rewind round-trips over the wire and the fork turn replaces the branch",
          "[session_service][rewind]") {
    ServiceFixture fixture;
    fixture.Script(testing::TextResponse("one"));
    fixture.Script(testing::TextResponse("two"));
    fixture.Script(testing::TextResponse("edited reply"));
    fixture.Start();
    const std::string session_id = fixture.CreateSession();

    grpc::ClientContext context;
    auto stream = fixture.stub->OpenSession(&context);
    REQUIRE(stream->Write(ChatTurn(session_id, "first")));
    REQUIRE(WaitForTurnCompleted(*stream));
    REQUIRE(stream->Write(ChatTurn(session_id, "second")));
    REQUIRE(WaitForTurnCompleted(*stream));
    // seqs: first=0, re=1, second=2, re=3.

    v1::ClientEvent rewind;
    rewind.set_session_id(session_id);
    rewind.mutable_rewind()->set_target_seq(2);
    REQUIRE(stream->Write(rewind));

    // The rewind's answer comes on the same stream. Read until it arrives,
    // failing on an error event -- silence here would hang the test.
    bool saw_rewound = false;
    v1::ServerEvent event;
    while (stream->Read(&event)) {
        if (event.has_session_rewound()) {
            saw_rewound = true;
            CHECK(event.session_rewound().truncated_to_seq() == 2);
            CHECK(event.session_rewound().active_last_seq() == 1);
            break;
        }
        REQUIRE_FALSE(event.has_error());
    }
    REQUIRE(saw_rewound);

    // THE FORK: the next turn reclaims the discarded branch and appends at
    // its seq.
    REQUIRE(stream->Write(ChatTurn(session_id, "edited")));
    REQUIRE(WaitForTurnCompleted(*stream));
    (void)DrainAndFinish(stream);

    const auto page = fixture.db->GetMessagesPage(session_id, std::nullopt, 50);
    REQUIRE(page.messages.size() == 4);  // kept turn + fork turn (seed and reply)
    // The new seed continued the ACTIVE sequence: seq 2 is the edited prompt,
    // not the discarded "second".
    const auto seed_row = std::find_if(page.messages.begin(), page.messages.end(),
                                       [](const auto& stored) { return stored.seq == 2; });
    REQUIRE(seed_row != page.messages.end());
    const auto& seed = seed_row->message;
    const auto* text = std::get_if<llm::TextBlock>(&seed.content.front());
    REQUIRE(text != nullptr);
    CHECK(text->text == "edited");

    // And the fork turn ran on the truncated context: the kept prefix (2) plus
    // its own seed -- no trace of the discarded branch.
    REQUIRE(fixture.script->requests.size() == 3);
    const auto& sent = fixture.script->requests.back().messages;
    REQUIRE(sent.size() == 3);
    const auto* first_text = std::get_if<llm::TextBlock>(&sent[0].content.front());
    REQUIRE(first_text != nullptr);
    CHECK(first_text->text == "first");
}

TEST_CASE("a redo over the wire restores the rewound tail", "[session_service][rewind]") {
    ServiceFixture fixture;
    fixture.Script(testing::TextResponse("one"));
    fixture.Script(testing::TextResponse("two"));
    fixture.Script(testing::TextResponse("back on the branch"));
    fixture.Start();
    const std::string session_id = fixture.CreateSession();

    grpc::ClientContext context;
    auto stream = fixture.stub->OpenSession(&context);
    REQUIRE(stream->Write(ChatTurn(session_id, "first")));
    REQUIRE(WaitForTurnCompleted(*stream));
    REQUIRE(stream->Write(ChatTurn(session_id, "second")));
    REQUIRE(WaitForTurnCompleted(*stream));

    v1::ClientEvent rewind;
    rewind.set_session_id(session_id);
    rewind.mutable_rewind()->set_target_seq(2);
    REQUIRE(stream->Write(rewind));

    bool saw_rewound = false;
    v1::ServerEvent event;
    while (stream->Read(&event)) {
        if (event.has_session_rewound()) {
            saw_rewound = true;
            break;
        }
        REQUIRE_FALSE(event.has_error());
    }
    REQUIRE(saw_rewound);

    v1::ClientEvent redo;
    redo.set_session_id(session_id);
    redo.mutable_rewind()->set_redo(true);
    REQUIRE(stream->Write(redo));

    saw_rewound = false;
    while (stream->Read(&event)) {
        if (event.has_session_rewound()) {
            saw_rewound = true;
            CHECK(event.session_rewound().truncated_to_seq() == -1);
            CHECK(event.session_rewound().active_last_seq() == 3);
            break;
        }
        REQUIRE_FALSE(event.has_error());
    }
    REQUIRE(saw_rewound);

    // The restored branch serves the next turn in full.
    REQUIRE(stream->Write(ChatTurn(session_id, "continue")));
    REQUIRE(WaitForTurnCompleted(*stream));
    (void)DrainAndFinish(stream);

    CHECK(fixture.db->GetMessagesPage(session_id, std::nullopt, 50).messages.size() == 6);
    REQUIRE(fixture.script->requests.size() == 3);
    CHECK(fixture.script->requests.back().messages.size() == 5);
}

TEST_CASE("an invalid rewind target is refused on the wire",
          "[session_service][rewind]") {
    ServiceFixture fixture;
    fixture.Script(testing::TextResponse("one"));
    fixture.Script(testing::TextResponse("still here"));
    fixture.Start();
    const std::string session_id = fixture.CreateSession();

    grpc::ClientContext context;
    auto stream = fixture.stub->OpenSession(&context);
    REQUIRE(stream->Write(ChatTurn(session_id, "first")));
    REQUIRE(WaitForTurnCompleted(*stream));
    // seq 1 is the assistant's reply, not a prompt to re-enter.

    v1::ClientEvent rewind;
    rewind.set_session_id(session_id);
    rewind.mutable_rewind()->set_target_seq(1);
    REQUIRE(stream->Write(rewind));

    bool saw_error = false;
    v1::ServerEvent event;
    while (stream->Read(&event)) {
        if (event.has_error()) {
            saw_error = true;
            CHECK(event.error().message().find("not an active user message") !=
                  std::string::npos);
            break;
        }
        REQUIRE_FALSE(event.has_session_rewound());
    }
    CHECK(saw_error);

    // The session survives a refused rewind and keeps accepting turns.
    REQUIRE(stream->Write(ChatTurn(session_id, "still here")));
    REQUIRE(WaitForTurnCompleted(*stream));
    (void)DrainAndFinish(stream);
    CHECK(fixture.db->GetMessagesPage(session_id, std::nullopt, 50).messages.size() == 4);
}

// --- Signals (channels + cross-session) over the wire -----------------------

TEST_CASE("signals: an unknown selector refuses the session start",
          "[session_service][signals]") {
    ServiceFixture fixture;
    std::string register_error;
    REQUIRE(fixture.signal_router.RegisterExternalChannel(
        agent::ExternalChannel{"ci", false, {"build.completed"}}, nullptr, register_error));
    fixture.Start();

    auto create = [&](const std::string& selector) {
        grpc::ClientContext context;
        v1::CreateSessionRequest request;
        request.set_project_root((fixture.dir / "work").string());
        request.set_agent_preset("main");
        request.add_signals(selector);
        v1::CreateSessionResponse response;
        const auto status = fixture.stub->CreateSession(&context, request, &response);
        return status.error_code();
    };

    // Unknown channel.
    CHECK(create("nowhere/x") == grpc::StatusCode::INVALID_ARGUMENT);
    // Declared channel, undeclared signal: fail-loud config drift, at start.
    CHECK(create("ci/build.unknown") == grpc::StatusCode::INVALID_ARGUMENT);
    // Restricted channels subscribe per signal.
    CHECK(create("ci") == grpc::StatusCode::INVALID_ARGUMENT);
    // Declared, restricted, exact: fine.
    CHECK(create("ci/build.completed") == grpc::StatusCode::OK);
}

TEST_CASE("signals: a subscribed session wakes and receives the message",
          "[session_service][signals]") {
    ServiceFixture fixture;
    fixture.Start();

    const std::string session_id = fixture.CreateSessionWithSignals({"sessions"});

    // Attach obligation: name the stream before any signal can arrive.
    grpc::ClientContext open_context;
    auto stream = fixture.stub->OpenSession(&open_context);
    v1::ClientEvent attach;
    attach.set_session_id(session_id);
    attach.mutable_attach();
    REQUIRE(stream->Write(attach));

    // An in-process producer sends to this session; the session is idle and
    // must WAKE (subscriptions force wake_on_injection) and run a turn whose
    // user content is the provenance-wrapped signal.
    fixture.Script(testing::TextResponse("on it"));
    CHECK(fixture.signal_router.DeliverInternal("peer-session", session_id,
                                                "deploy finished, please verify") ==
          agent::SignalDelivery::kDelivered);
    REQUIRE(WaitForTurnCompleted(*stream));
    (void)DrainAndFinish(stream);

    const auto page = fixture.db->GetMessagesPage(session_id, std::nullopt, 50);
    bool saw_signal = false;
    for (const auto& stored : page.messages) {
        for (const auto& block : stored.message.content) {
            if (auto* text = std::get_if<llm::TextBlock>(&block)) {
                if (text->text.find("deploy finished, please verify") != std::string::npos &&
                    text->text.find("<signal source=\"sessions\" from=\"peer-session\">") !=
                        std::string::npos) {
                    saw_signal = true;
                }
            }
        }
    }
    CHECK(saw_signal);
}

TEST_CASE("signals: no subscriber, no delivery -- and an extended channel passes every signal",
          "[session_service][signals]") {
    ServiceFixture fixture;
    std::string register_error;
    REQUIRE(fixture.signal_router.RegisterExternalChannel(
        agent::ExternalChannel{"reminder", true, {}}, nullptr, register_error));
    fixture.Start();

    // A session with NO subscriptions: nothing is delivered to it, even though
    // it is running.
    const std::string unsubscriber = fixture.CreateSession();
    fixture.Script(testing::TextResponse("ack"));

    // Channel-level subscription to an extended channel.
    const std::string subscriber = fixture.CreateSessionWithSignals({"reminder"});

    grpc::ClientContext open_context;
    auto stream = fixture.stub->OpenSession(&open_context);
    v1::ClientEvent attach;
    attach.set_session_id(subscriber);
    attach.mutable_attach();
    REQUIRE(stream->Write(attach));

    fixture.Script(testing::TextResponse("noted"));
    CHECK(fixture.signal_router.DeliverInternal("someone", unsubscriber,
                                                "you did not subscribe") ==
          agent::SignalDelivery::kNoSubscriber);
    // The EXTERNAL producer path (core.signals' ingress, what the MCP
    // channel transport feeds): an extended channel delivers any signal name.
    CHECK(fixture.signal_router.DeliverExternal("reminder", "any.thing", "extended passes", {}) ==
          agent::SignalDelivery::kDelivered);
    REQUIRE(WaitForTurnCompleted(*stream));
    (void)DrainAndFinish(stream);

    const auto page = fixture.db->GetMessagesPage(subscriber, std::nullopt, 50);
    bool saw_extended = false;
    for (const auto& stored : page.messages) {
        for (const auto& block : stored.message.content) {
            if (auto* text = std::get_if<llm::TextBlock>(&block)) {
                if (text->text.find("extended passes") != std::string::npos) saw_extended = true;
            }
        }
    }
    CHECK(saw_extended);
}
