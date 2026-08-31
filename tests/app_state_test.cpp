// e2e tier for AppState/SessionClient::Stream: a real rpc::Server + FakeLlmProvider
// behind a real unix-socket gRPC channel, driven through the actual SessionClient
// used by the TUI. AppState has no seam around SessionClient, so this cannot be a
// pure-logic test -- see tests/session_state_test.cpp for that tier.
//
// The fixture below is a trimmed duplicate of session_service_test.cpp's
// ServiceFixture: only what a client-side test needs (script a response, Start(),
// a dialable target) rather than the server-internal handles (stub/db) client
// tests have no reason to touch.

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "deadline.hpp"
#include "fakes.hpp"
#include "minimal/client/session_client.hpp"
#include "minimal/client/state.hpp"
#include "minimal/rpc/server.hpp"
#include "minimal/rpc/session_service.hpp"

using namespace minimal;
using namespace std::chrono_literals;
using minimal::client::AppState;
using minimal::client::SessionClient;
using minimal::client::SessionState;
using minimal::client::SessionStateKind;

namespace {

struct ClientFixture {
    ClientFixture() {
        dir = std::filesystem::temp_directory_path() / "minimal_app_state_test_arena";
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

        skills = config::SkillRegistry::Load(dir);
    }

    void Write(const std::filesystem::path& relative, const std::string& contents) const {
        const auto path = dir / relative;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path) << contents;
    }

    // One scripted text response, replayed by every session's fake provider.
    void Script(const std::string& text) {
        providers.Register("fake", [text](const llm::ProviderConfig&) {
            auto provider = std::make_unique<testing::FakeLlmProvider>();
            provider->PushResponse(
                testing::TextResponse(text),
                {llm::StreamEvent{llm::MessageStartEvent{"m-1", "test-model"}},
                 llm::StreamEvent{llm::MessageStopEvent{}}});
            return provider;
        });
    }

    void Start() {
        presets = std::make_unique<agent::AgentPresetRegistry>(dir, skills);
        observers = std::make_unique<agent::SessionObserverRegistry>(common::DefaultLogger());
        db = std::make_unique<session::SessionDatabase>(dir / "sessions.db");
        sessions = std::make_unique<agent::SessionRegistry>(
            *presets, tools, providers, *observers, *db, agent::LoopConfig{},
            common::DefaultLogger(), attach_guard);
        service = std::make_unique<rpc::SessionServiceImpl>(
            *sessions, *db, config::SettingsConfig{}, config::PermissionsConfig{},
            common::DefaultLogger());

        socket = dir / "test.sock";
        server = std::make_unique<rpc::Server>(*service, socket);
        REQUIRE(server->listening());

        client = std::make_shared<SessionClient>(rpc::SocketTarget(socket));
        REQUIRE(client->WaitForConnected(5s));
    }

    SessionClient::CreateOptions Options(const std::string& name = "") const {
        SessionClient::CreateOptions opts;
        opts.project_root = (dir / "work").string();
        opts.agent_preset = "main";
        opts.name = name;
        return opts;
    }

    ~ClientFixture() {
        client.reset();
        if (server) server->Shutdown();
        server.reset();
        if (sessions) sessions->TerminateAll();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    std::filesystem::path dir, socket;
    config::SkillRegistry skills;
    tools::ToolRegistry tools;
    llm::ProviderFactoryRegistry providers;
    std::unique_ptr<agent::AgentPresetRegistry> presets;
    std::unique_ptr<agent::SessionObserverRegistry> observers;
    std::unique_ptr<session::SessionDatabase> db;
    std::unique_ptr<agent::SessionRegistry> sessions;
    std::unique_ptr<rpc::SessionServiceImpl> service;
    std::unique_ptr<rpc::Server> server;
    std::shared_ptr<SessionClient> client;
    std::chrono::milliseconds attach_guard = std::chrono::seconds(10);
};

// Polls a tab's snapshot until `pred` is true or the deadline trips. AppState is
// updated from a reader thread, so tests must wait rather than assert immediately.
template <typename Pred>
bool WaitFor(AppState& app, const std::string& session_id, Pred pred,
             std::chrono::milliseconds timeout = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        for (const auto& tab : app.GetAllTabs()) {
            if (tab.session_id == session_id && pred(tab)) return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return false;
}

}  // namespace

TEST_CASE("OpenTab creates a real session and registers a tab", "[app_state]") {
    ClientFixture fixture;
    fixture.Script("hi");
    fixture.Start();

    AppState app(fixture.client);
    const auto sid = app.OpenTab(fixture.Options("first"));
    REQUIRE_FALSE(sid.empty());
    REQUIRE(app.HasTab(sid));

    auto tabs = app.GetAllTabs();
    REQUIRE(tabs.size() == 1);
    CHECK(tabs[0].session_id == sid);
    CHECK(tabs[0].is_focused);
}

TEST_CASE("two tabs do not cross-talk", "[app_state]") {
    ClientFixture fixture;
    fixture.Script("scripted reply");
    fixture.Start();

    AppState app(fixture.client);
    const auto sid_a = app.OpenTab(fixture.Options("a"));
    const auto sid_b = app.OpenTab(fixture.Options("b"));
    REQUIRE_FALSE(sid_a.empty());
    REQUIRE_FALSE(sid_b.empty());

    app.SendChatTurn(sid_a, "hello from a");
    REQUIRE(WaitFor(app, sid_a, [](const AppState::TabSnapshot& t) {
        return t.state == SessionStateKind::Idle || t.state == SessionStateKind::Thinking ||
               t.state == SessionStateKind::Working;
    }));
    // Give the turn a moment to actually land before checking B was untouched --
    // WaitFor above only proves A started moving, not that the turn finished.
    std::this_thread::sleep_for(200ms);

    bool b_touched = false;
    for (const auto& tab : app.GetAllTabs()) {
        if (tab.session_id == sid_b) b_touched = (tab.state != SessionStateKind::Idle);
    }
    CHECK_FALSE(b_touched);
}

TEST_CASE("AttachTab resumes a session created out-of-band", "[app_state]") {
    ClientFixture fixture;
    fixture.Script("hi");
    fixture.Start();

    const auto created = fixture.client->CreateSession(fixture.Options("resumed"));
    REQUIRE(created.status.ok());
    REQUIRE_FALSE(created.session_id.empty());

    AppState app(fixture.client);
    const auto sid = app.AttachTab(created.session_id);
    REQUIRE(sid == created.session_id);
    REQUIRE(app.HasTab(created.session_id));
}

TEST_CASE("subagent grouping and focus via OnSubagentSpawned", "[app_state]") {
    ClientFixture fixture;
    fixture.Script("hi");
    fixture.Start();

    AppState app(fixture.client);
    const auto parent = app.OpenTab(fixture.Options("parent"));
    REQUIRE_FALSE(parent.empty());

    // No real subagent spawning on this branch yet -- stand in with a second,
    // independently-created real session and drive AppState's own grouping/focus
    // logic directly, which is what this test actually exercises.
    const auto created = fixture.client->CreateSession(fixture.Options("child"));
    REQUIRE(created.status.ok());

    v1::SubagentStartedEvent blocking_evt;
    blocking_evt.set_child_session_id(created.session_id);
    blocking_evt.set_blocking(true);
    app.OnSubagentSpawned(parent, blocking_evt);

    REQUIRE(app.HasTab(created.session_id));
    auto tabs = app.GetAllTabs();
    const AppState::TabSnapshot* parent_snap = nullptr;
    const AppState::TabSnapshot* child_snap = nullptr;
    for (const auto& t : tabs) {
        if (t.session_id == parent) parent_snap = &t;
        if (t.session_id == created.session_id) child_snap = &t;
    }
    REQUIRE(parent_snap != nullptr);
    REQUIRE(child_snap != nullptr);
    CHECK(child_snap->is_subagent);
    CHECK(child_snap->root_ordinal == parent_snap->root_ordinal);
    CHECK(child_snap->is_focused);     // blocking -> auto-focus
    CHECK_FALSE(parent_snap->is_focused);
}

TEST_CASE("a non-blocking subagent attaches without moving focus", "[app_state]") {
    ClientFixture fixture;
    fixture.Script("hi");
    fixture.Start();

    AppState app(fixture.client);
    const auto parent = app.OpenTab(fixture.Options("parent"));
    REQUIRE_FALSE(parent.empty());
    REQUIRE(app.GetAllTabs()[0].is_focused);

    const auto created = fixture.client->CreateSession(fixture.Options("child"));
    REQUIRE(created.status.ok());

    v1::SubagentStartedEvent nonblocking_evt;
    nonblocking_evt.set_child_session_id(created.session_id);
    nonblocking_evt.set_blocking(false);
    app.OnSubagentSpawned(parent, nonblocking_evt);

    REQUIRE(app.HasTab(created.session_id));
    for (const auto& t : app.GetAllTabs()) {
        if (t.session_id == parent) CHECK(t.is_focused);
        if (t.session_id == created.session_id) CHECK_FALSE(t.is_focused);
    }
}

TEST_CASE("CloseTab removes the tab and reassigns focus", "[app_state]") {
    ClientFixture fixture;
    fixture.Script("hi");
    fixture.Start();

    AppState app(fixture.client);
    const auto sid_a = app.OpenTab(fixture.Options("a"));
    const auto sid_b = app.OpenTab(fixture.Options("b"));
    REQUIRE_FALSE(sid_a.empty());
    REQUIRE_FALSE(sid_b.empty());
    app.SetFocusedTab(sid_b);

    app.CloseTab(sid_b);
    REQUIRE_FALSE(app.HasTab(sid_b));
    auto tabs = app.GetAllTabs();
    REQUIRE(tabs.size() == 1);
    CHECK(tabs[0].session_id == sid_a);
    CHECK(tabs[0].is_focused);   // closing the focused tab reassigns focus
}

TEST_CASE("SessionClient::GetMessages feeds ApplyScrollbackPage with real wire data",
          "[app_state][scrollback]") {
    // Proves the client's GetMessages wrapper and SessionState::ApplyScrollbackPage
    // agree on the wire format -- mirrors "a turn is durable and pages back through
    // GetMessages" (session_service_test.cpp), but driven from the client side.
    ClientFixture fixture;
    fixture.Script("hello from the model");
    fixture.Start();

    const auto created = fixture.client->CreateSession(fixture.Options("durable"));
    REQUIRE(created.status.ok());

    const auto turn_status = fixture.client->RunTurn(
        created.session_id, "hi", [](const v1::ServerEvent&) { return true; });
    REQUIRE(turn_status.ok());

    const auto page = fixture.client->GetMessages(created.session_id, std::nullopt, 10);
    REQUIRE(page.status.ok());
    REQUIRE_FALSE(page.response.messages().empty());

    SessionState state(created.session_id);
    state.ApplyScrollbackPage(page.response);
    auto snap = state.TakeSnapshot();
    CHECK(snap.scrollback_page.size() == static_cast<size_t>(page.response.messages_size()));
    CHECK(snap.last_loaded_seq == page.response.messages(page.response.messages_size() - 1).seq());
}

TEST_CASE("a session terminated server-side (not via CloseTab) reaches Done",
          "[app_state]") {
    // OnSessionEnded matters precisely when the tab is still present but its
    // session ended on its own -- the real-world case is a subagent finishing
    // while its tab stays open for the user to review (see ARCHITECTURE.md,
    // "Done | subagent session stopped"). CloseTab erases the tab entry itself,
    // so it can never observe this transition; this test terminates the session
    // out from under a still-registered tab instead, standing in for that case.
    ClientFixture fixture;
    fixture.Script("hi");
    fixture.Start();

    AppState app(fixture.client);
    const auto sid = app.OpenTab(fixture.Options("solo"));
    REQUIRE_FALSE(sid.empty());
    REQUIRE(app.HasTab(sid));

    fixture.sessions->Terminate(sid);   // server-side; the tab is untouched by AppState

    REQUIRE(WaitFor(app, sid, [](const AppState::TabSnapshot& t) {
        return t.state == SessionStateKind::Done;
    }));
    CHECK(app.HasTab(sid));   // the tab itself is never removed automatically
}
