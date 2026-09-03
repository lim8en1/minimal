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

#include <unistd.h>

#include <catch2/catch_test_macros.hpp>

#include "../plugins/server/commands-core/core_commands.hpp"
#include "deadline.hpp"
#include "fakes.hpp"
#include "minimal/client/session_client.hpp"
#include "minimal/client/state.hpp"
#include "minimal/commands/command_registry.hpp"
#include "minimal/config/settings_config.hpp"
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
        // Unique per PROCESS: ctest runs every test case as its own invocation
        // of this binary, in parallel. A fixed shared arena turned the
        // destructor's remove_all into a race -- one fixture deleting
        // settings.json out from under a concurrently starting one, surfacing
        // as a flaky "settings.json not found" from OpenTab. The same session_id
        // collision would be worse, so the directory name is unique instead.
        dir = std::filesystem::temp_directory_path() /
              ("minimal_app_state_test_arena_" + std::to_string(::getpid()));
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
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

    // Multi-turn scripts: each session gets a FRESH provider replaying the
    // steps in order (the fixture's factory runs per session create).
    void ScriptSteps(const std::vector<llm::ChatResponse>& steps) {
        providers.Register("fake", [steps](const llm::ProviderConfig&) {
            auto provider = std::make_unique<testing::FakeLlmProvider>();
            for (const auto& step : steps) {
                provider->PushResponse(step);
            }
            return provider;
        });
    }

    // Two text responses on ONE provider instance. The registry CACHES the
    // provider per providers.json entry (session_registry.cpp: "SHARED, not
    // per-session"), so a factory registered AFTER a session exists never
    // reaches it -- re-scripting between phases does nothing for a resumed
    // session. Consuming both responses from the shared instance is how a
    // resume test scripts its second phase.
    void ScriptTwo(const std::string& first, const std::string& second) {
        providers.Register("fake", [first, second](const llm::ProviderConfig&) {
            auto provider = std::make_unique<testing::FakeLlmProvider>();
            for (const auto* text : {&first, &second}) {
                provider->PushResponse(
                    testing::TextResponse(*text),
                    {llm::StreamEvent{llm::MessageStartEvent{"m-1", "test-model"}},
                     llm::StreamEvent{llm::MessageStopEvent{}}});
            }
            return provider;
        });
    }

    void Start() {
        presets = std::make_unique<agent::AgentPresetRegistry>(
            dir, skills, config::LoadSettingsConfig(dir / "settings.json"));
        observers = std::make_unique<agent::SessionObserverRegistry>(common::DefaultLogger());
        db = std::make_unique<session::SessionDatabase>(dir / "sessions.db");
        for (auto& command : commands_core::MakeCoreCommands()) commands.Register(std::move(command));
        sessions = std::make_unique<agent::SessionRegistry>(
            *presets, tools, providers, *observers, *db, agent::LoopConfig{},
            common::DefaultLogger(), attach_guard);
        sessions->set_commands(&commands);
        service = std::make_unique<rpc::SessionServiceImpl>(
            *sessions, *db, config::SettingsConfig{}, config::PermissionsConfig{},
            common::DefaultLogger());
        service->set_commands(&commands);

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
    commands::CommandRegistry commands;
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

TEST_CASE("AttachTab does not itself steal focus; callers must SetFocusedTab", "[app_state]") {
    // AttachSubagent relies on AttachTab NOT auto-focusing (it only steals
    // focus for BLOCKING subagents, via its own explicit SetFocusedTab), so
    // AttachTab's `if (focused_id_.empty())` guard is intentional -- but that
    // means a picker resume while a tab is already focused registers the new
    // tab invisibly unless ITS caller focuses it explicitly. Regression test
    // for that "resume does nothing" bug: with something already focused,
    // AttachTab alone must not move focus, and SetFocusedTab must.
    ClientFixture fixture;
    fixture.Script("hi");
    fixture.Start();

    AppState app(fixture.client);
    const auto first = app.OpenTab(fixture.Options("first"));
    REQUIRE_FALSE(first.empty());

    const auto created = fixture.client->CreateSession(fixture.Options("resumed"));
    REQUIRE(created.status.ok());

    const auto sid = app.AttachTab(created.session_id);
    REQUIRE(sid == created.session_id);
    REQUIRE(app.HasTab(created.session_id));

    bool resumed_focused = false;
    for (const auto& tab : app.GetAllTabs()) {
        if (tab.session_id == sid) resumed_focused = tab.is_focused;
    }
    CHECK_FALSE(resumed_focused);   // AttachTab alone leaves the original tab focused

    app.SetFocusedTab(sid);
    resumed_focused = false;
    for (const auto& tab : app.GetAllTabs()) {
        if (tab.session_id == sid) resumed_focused = tab.is_focused;
    }
    CHECK(resumed_focused);
}

TEST_CASE("FindRewindTargetSeq resolves the Nth user turn back", "[app_state][rewind]") {
    // Rewind has no other way to let a user target it -- raw transcript seq
    // numbers are never surfaced in the UI, so /rewind <n> resolves "n user
    // turns back" to a seq via this scan. Two turns give two ROLE_USER rows
    // to count back through; a third request (past the loaded window) must
    // come back empty rather than a stale or out-of-range seq.
    ClientFixture fixture;
    fixture.ScriptSteps({testing::TextResponse("first reply"), testing::TextResponse("second reply")});
    fixture.Start();

    AppState app(fixture.client);
    const auto sid = app.OpenTab(fixture.Options("rewind-target"));
    REQUIRE_FALSE(sid.empty());

    app.SendChatTurn(sid, "first message");
    REQUIRE(WaitFor(app, sid, [](const AppState::TabSnapshot& t) {
        return t.state == SessionStateKind::Idle;
    }));
    app.SendChatTurn(sid, "second message");
    REQUIRE(WaitFor(app, sid, [](const AppState::TabSnapshot& t) {
        return t.state == SessionStateKind::Idle;
    }));

    // RequestScrollback is async and fetches a snapshot at whatever point the
    // worker gets to it -- a single call raced against the two turns above
    // landing server-side can legitimately fetch an empty page (WaitFor only
    // proves the tab observed Idle, not that persistence finished). Re-issue
    // it each retry so the eventual fetch sees the settled transcript -- and
    // gate on the SECOND user turn: gating on the first can break the loop
    // while turn 2 is still in flight, leaving the page one turn short.
    std::optional<int64_t> older_user_seq;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        app.RequestScrollback(sid, /*newest=*/true);
        std::this_thread::sleep_for(50ms);
        older_user_seq = app.FindRewindTargetSeq(sid, 2);
        if (older_user_seq.has_value()) break;
    }
    REQUIRE(older_user_seq.has_value());

    const auto newest_user_seq = app.FindRewindTargetSeq(sid, 1);
    REQUIRE(newest_user_seq.has_value());
    CHECK(*older_user_seq < *newest_user_seq);

    CHECK_FALSE(app.FindRewindTargetSeq(sid, 3).has_value());   // only two user turns exist
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

// Regression: CloseTab used to jump focus to tabs_.back() unconditionally,
// regardless of which tab actually closed -- closing tab 2 of 4 landed
// focus on tab 4, not on either of tab 2's real neighbors. Four tabs so
// "the tab that shifted into the closed slot" (b's neighbor, c) and
// "tabs_.back()" (d) are DIFFERENT tabs -- with only 3 tabs the two rules
// coincide and this could not have caught the bug.
TEST_CASE("CloseTab on a MIDDLE tab focuses its neighbor, not tabs_.back()",
         "[app_state][regression]") {
    ClientFixture fixture;
    fixture.Script("hi");
    fixture.Start();

    AppState app(fixture.client);
    const auto sid_a = app.OpenTab(fixture.Options("a"));
    const auto sid_b = app.OpenTab(fixture.Options("b"));
    const auto sid_c = app.OpenTab(fixture.Options("c"));
    const auto sid_d = app.OpenTab(fixture.Options("d"));
    REQUIRE_FALSE(sid_a.empty());
    REQUIRE_FALSE(sid_b.empty());
    REQUIRE_FALSE(sid_c.empty());
    REQUIRE_FALSE(sid_d.empty());
    app.SetFocusedTab(sid_b);

    app.CloseTab(sid_b);   // the middle tab, focused
    REQUIRE_FALSE(app.HasTab(sid_b));
    auto tabs = app.GetAllTabs();
    REQUIRE(tabs.size() == 3);
    CHECK(tabs[0].session_id == sid_a);
    CHECK(tabs[1].session_id == sid_c);
    CHECK(tabs[2].session_id == sid_d);
    CHECK(tabs[1].is_focused);      // c: the tab that shifted into b's slot
    CHECK_FALSE(tabs[2].is_focused);   // NOT d (tabs_.back()) -- the pre-fix bug
}

// The other half of the fix: closing the LAST tab has no "next" neighbor
// (erase() returns end()), so focus must fall back to the new last tab --
// the PREVIOUS one -- rather than leaving focused_id_ dangling on the
// just-closed id.
TEST_CASE("CloseTab on the LAST tab falls back to the new last tab",
         "[app_state][regression]") {
    ClientFixture fixture;
    fixture.Script("hi");
    fixture.Start();

    AppState app(fixture.client);
    const auto sid_a = app.OpenTab(fixture.Options("a"));
    const auto sid_b = app.OpenTab(fixture.Options("b"));
    const auto sid_c = app.OpenTab(fixture.Options("c"));
    REQUIRE_FALSE(sid_a.empty());
    REQUIRE_FALSE(sid_b.empty());
    REQUIRE_FALSE(sid_c.empty());
    app.SetFocusedTab(sid_c);

    app.CloseTab(sid_c);   // the last tab, focused -- erase() returns end()
    REQUIRE_FALSE(app.HasTab(sid_c));
    auto tabs = app.GetAllTabs();
    REQUIRE(tabs.size() == 2);
    CHECK(tabs[1].session_id == sid_b);
    CHECK(tabs[1].is_focused);
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

// Polls a tab's DEEP snapshot until `pred` is true or the deadline trips (the
// pending-modal data rides GetDetails, not the light tab list).
template <typename Pred>
bool WaitForDetails(AppState& app, const std::string& session_id, Pred pred,
                    std::chrono::milliseconds timeout = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred(app.GetDetails(session_id))) return true;
        std::this_thread::sleep_for(5ms);
    }
    return false;
}

// Waits until the tab has actually ATTACHED -- the server emits its metadata
// event to the new subscriber immediately on attach, so a non-empty
// agent_preset proves the naming event won its race against thread startup.
// Anything that terminates the session server-side MUST wait for this first:
// terminating a session the stream never attached to answers the bidi with
// NOT_FOUND, which is the attach-refusal path, not a clean end.
bool WaitForAttached(AppState& app, const std::string& session_id,
                     std::chrono::milliseconds timeout = 5s) {
    return WaitForDetails(app, session_id, [](const SessionState::Snapshot& s) {
        return !s.agent_preset.empty();
    }, timeout);
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

    REQUIRE(WaitForAttached(app, sid));
    fixture.sessions->Terminate(sid);   // server-side; the tab is untouched by AppState

    // The wire SessionEndedEvent (published before the server closes the
    // subscriber queues) routes to OnSessionEnded -- the stream itself stays
    // open, so the tab must not depend on a stream close to reach Done.
    REQUIRE(WaitFor(app, sid, [](const AppState::TabSnapshot& t) {
        return t.state == SessionStateKind::Done;
    }));
    CHECK(app.HasTab(sid));   // the tab itself is never removed automatically
}

// Track R1b-4 (see .opencode/plans/track-r-remediation.md): DispatchToTab used
// to grab a RAW SessionState* under the lock and use it after releasing it.
// CloseTab erases the TabEntry -- dropping the map's shared_ptr -- and only
// then joins the reader thread, so a dispatch in flight at that moment was a
// use-after-free on every tab closed during a live turn. The owning shared_ptr
// copy makes this structurally safe; with the raw pointer restored, this test
// is an ASan finding (run under the asan preset -- the window is real, not
// theoretical: the turn's stream events are arriving while the erase lands).
TEST_CASE("INVARIANT: closing a tab while its turn streams cannot use-after-free "
          "the dispatched state",
          "[app_state][invariant]") {
    ClientFixture fixture;
    fixture.Script("hi");
    fixture.Start();

    AppState app(fixture.client);
    for (int i = 0; i < 8; ++i) {
        const auto sid = app.OpenTab(fixture.Options("close-race-" + std::to_string(i)));
        REQUIRE_FALSE(sid.empty());
        app.SendChatTurn(sid, "hi");   // stream events flowing on the reader thread
        app.CloseTab(sid);             // erase + join while dispatch is in flight
        REQUIRE_FALSE(app.HasTab(sid));
    }
    CHECK(app.GetAllTabs().empty());
}

// ---------------------------------------------------------------------------
// The interactive verbs: interrupt (SendCancel) and the wire confirmation
// answer. Both round-trip over a real socket through the real dispatch path
// the TUI uses -- the client-side half of session_service_test's coverage.
// ---------------------------------------------------------------------------

TEST_CASE("SendCancel ends a busy turn and the session stays usable", "[app_state][cancel]") {
    // An Ask-gated tool parks the turn at a deterministic point (the same
    // rendezvous session_service_test uses); the cancel instead of an answer.
    ClientFixture fixture;
    fixture.tools.Register(
        std::make_unique<testing::FakeTool>("bash", tools::ToolCategory::Execute, "ran it"));
    fixture.ScriptSteps({testing::ToolUseResponse("call-1", "bash", {{"command", "make"}}),
                         testing::TextResponse("second turn finished")});
    fixture.Write("permissions.json", nlohmann::json{{"fallback", {{"execute", "ask"}}}}.dump());
    fixture.Start();

    AppState app(fixture.client);
    const auto sid = app.OpenTab(fixture.Options("cancel-me"));
    REQUIRE_FALSE(sid.empty());

    app.SendChatTurn(sid, "build it");
    REQUIRE(WaitForDetails(app, sid,
                           [](const SessionState::Snapshot& s) {
                               return s.pending_permission.has_value();
                           }));
    CHECK(app.GetDetails(sid).state == SessionStateKind::Waiting);

    // THE VERB: the server ends the turn with STOP_REASON_CANCELLED and
    // answers the parked prompt with a denial (pending.CancelAll). Before
    // this was wired the TUI had no interrupt at all.
    app.SendCancel(sid);
    CHECK(WaitFor(app, sid, [](const AppState::TabSnapshot& t) {
        return t.state == SessionStateKind::Idle;
    }));
    CHECK_FALSE(app.GetDetails(sid).cancel_requested);   // retired by the turn end
    CHECK_FALSE(app.GetDetails(sid).pending_permission.has_value());

    // A cancel must not poison the session: turn 2 runs to completion.
    app.SendChatTurn(sid, "try again");
    CHECK(WaitFor(app, sid, [](const AppState::TabSnapshot& t) {
        return t.state == SessionStateKind::Idle;
    }));
}

TEST_CASE("a wire confirmation is answered and unwedges the tab", "[app_state][confirmation]") {
    // /clear asks through the ConfirmationBroker, which BLOCKS the session
    // worker until answered. The TUI's AnswerConfirmation existed and was
    // dead code: nothing rendered the request, so the tab sat in Waiting
    // forever -- a correctness bug, not a missing nicety.
    ClientFixture fixture;
    fixture.Script("hi");
    fixture.Start();

    AppState app(fixture.client);
    const auto sid = app.OpenTab(fixture.Options("confirm-me"));
    REQUIRE_FALSE(sid.empty());

    app.SendCommand(sid, "/clear");
    REQUIRE(WaitFor(app, sid, [](const AppState::TabSnapshot& t) {
        return t.state == SessionStateKind::Waiting;
    }));

    const auto pending = app.GetDetails(sid).pending_confirmation;
    REQUIRE(pending.has_value());
    REQUIRE(pending->options.size() == 2);   // the dispatcher's {"Yes", "No"}

    app.AnswerConfirmation(sid, pending->request_id, "Yes");

    // THE WEDGE FIX: no tool events or message stops follow a command
    // confirmation, so the state must re-derive at the answer -- not sit in
    // Waiting until some unrelated event happens to arrive.
    CHECK(WaitFor(app, sid, [](const AppState::TabSnapshot& t) {
        return t.state == SessionStateKind::Idle;
    }));
    // The command actually ran: its result comes back as the notice.
    CHECK(WaitForDetails(app, sid, [](const SessionState::Snapshot& s) {
        return s.latest_notice.has_value();
    }));
}

// ---------------------------------------------------------------------------
// ResumeTab: the picker's "open a past session" action. The regression this
// covers is THE session-resume bug: the picker used to bare-attach, the
// server answered NOT_FOUND for any stored (non-live) session, and the dead
// stream silently swallowed every prompt and /command -- while the tab still
// RENDERED its history, because scrollback reads the database directly.
// ---------------------------------------------------------------------------

TEST_CASE("ResumeTab revives a stored session and its tab sends again",
          "[app_state][resume]") {
    ClientFixture fixture;
    fixture.ScriptTwo("first reply", "second reply");
    fixture.Start();

    AppState app(fixture.client);
    const auto sid = app.OpenTab(fixture.Options("resume-me"));
    REQUIRE_FALSE(sid.empty());

    // A bare state-Idle wait passes instantly (the tab is Idle BEFORE the
    // turn's first event lands), so turn completion is polled through the
    // transcript itself: the turn is over exactly when its rows are durable.
    const auto wait_rows = [&app](const std::string& sid, size_t n) {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline) {
            app.RequestScrollback(sid, /*newest=*/true);
            std::this_thread::sleep_for(50ms);
            if (app.GetDetails(sid).scrollback_page.size() >= n) return true;
        }
        return false;
    };

    app.SendChatTurn(sid, "first message");
    REQUIRE(wait_rows(sid, 2));   // user + assistant, persisted

    // TERMINATE (not detach): the transcript stays in SQLite, but the LIVE
    // session must be gone -- ResumeSession refuses an already-open session.
    // CloseTab drops the client side; the registry drop is polled.
    app.CloseTab(sid);
    fixture.sessions->Terminate(sid);
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (fixture.sessions->Find(sid) != nullptr &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    REQUIRE(fixture.sessions->Find(sid) == nullptr);

    REQUIRE(app.ResumeTab(sid) == sid);
    REQUIRE(app.HasTab(sid));
    CHECK(app.GetAllTabs()[0].is_focused);   // the app was empty: focus-if-empty

    // THE BUG: a picked stored session must be LIVE, not a dead tab that
    // renders history but drops input.
    app.SendChatTurn(sid, "second message");
    REQUIRE(wait_rows(sid, 4));   // the resumed turn completed ON TOP of the stored transcript

    // History intact across the resume: all four rows in order, nothing
    // duplicated or renumbered (the client-side mirror of "a stored session
    // resumes with its history intact", session_service_test.cpp).
    const auto page = fixture.client->GetMessages(sid, std::nullopt, 10);
    REQUIRE(page.status.ok());
    REQUIRE(page.response.messages_size() == 4);
    CHECK(page.response.messages(3).message().content(0).text().text() == "first message");
    CHECK(page.response.messages(2).message().content(0).text().text() == "first reply");
    CHECK(page.response.messages(1).message().content(0).text().text() == "second message");
    CHECK(page.response.messages(0).message().content(0).text().text() == "second reply");
}

TEST_CASE("ResumeTab of an unknown session registers a visible error tab",
          "[app_state][resume]") {
    ClientFixture fixture;
    fixture.Script("hi");
    fixture.Start();

    AppState app(fixture.client);
    const auto ghost = "01ARZ3NDEKTSV4RRFFQ69G5FAV";   // well-formed, nonexistent

    // A refused resume still registers the tab -- in the Error state, with
    // the refusal as its notice -- instead of silently doing nothing.
    REQUIRE(app.ResumeTab(ghost) == ghost);
    REQUIRE(app.HasTab(ghost));
    CHECK(app.GetAllTabs()[0].is_focused);
    CHECK(WaitForDetails(app, ghost, [](const SessionState::Snapshot& s) {
        return s.latest_notice.has_value() && s.latest_notice->is_error;
    }));
    CHECK(app.GetDetails(ghost).state == SessionStateKind::Error);

    // And sends on that stream-less tab refuse VISIBLY, not silently.
    app.SendChatTurn(ghost, "hello?");
    CHECK(WaitForDetails(app, ghost, [](const SessionState::Snapshot& s) {
        return s.latest_notice.has_value() &&
               s.latest_notice->message.find("cannot send") != std::string::npos;
    }));
}

TEST_CASE("ResumeTab on an already-open tab focuses it instead of duplicating",
          "[app_state][resume]") {
    ClientFixture fixture;
    fixture.Script("hi");
    fixture.Start();

    AppState app(fixture.client);
    const auto sid_a = app.OpenTab(fixture.Options("a"));
    const auto sid_b = app.OpenTab(fixture.Options("b"));
    REQUIRE_FALSE(sid_a.empty());
    REQUIRE_FALSE(sid_b.empty());
    app.SetFocusedTab(sid_b);

    // The old picker path silently no-opped this pick (AttachTab refused the
    // duplicate and returned ""); the user's pick must at least land focus.
    // (Focus comes from the picker's own SetFocusedTab after ResumeTab.)
    REQUIRE(app.ResumeTab(sid_a) == sid_a);
    REQUIRE(app.GetAllTabs().size() == 2);
    app.SetFocusedTab(sid_a);
    for (const auto& t : app.GetAllTabs()) {
        if (t.session_id == sid_a) CHECK(t.is_focused);
        if (t.session_id == sid_b) CHECK_FALSE(t.is_focused);
    }
}

// The other half of the dead-stream fix: once a stream's reader has observed
// the end (Finish() ran), the stream is latched closed -- a Send() must be
// REFUSED (surfacing OnError) rather than accepted into a queue the writer's
// failed Write() then silently drains. The old behavior: typing into a
// session that ended server-side vanished without a trace.
TEST_CASE("sends on a stream that died with the session are refused visibly",
          "[app_state][resume]") {
    ClientFixture fixture;
    fixture.Script("hi");
    fixture.Start();

    AppState app(fixture.client);
    const auto sid = app.OpenTab(fixture.Options("dies"));
    REQUIRE_FALSE(sid.empty());
    REQUIRE(WaitForAttached(app, sid));   // terminate an ATTACHED session, not a refused bidi

    // Complete one turn (durable rows prove completion; a bare state-Idle
    // wait passes instantly -- the tab is Idle before the turn starts). The
    // fetch is re-issued per retry: nothing else drives scrollback refreshes
    // in a headless test.
    app.SendChatTurn(sid, "go");
    bool turn_done = false;
    const auto turn_deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < turn_deadline) {
        app.RequestScrollback(sid, /*newest=*/true);
        std::this_thread::sleep_for(50ms);
        if (app.GetDetails(sid).scrollback_page.size() >= 2) {
            turn_done = true;
            break;
        }
    }
    REQUIRE(turn_done);

    fixture.sessions->Terminate(sid);
    REQUIRE(WaitFor(app, sid, [](const AppState::TabSnapshot& t) {
        return t.state == SessionStateKind::Done;
    }));

    app.SendChatTurn(sid, "anyone there?");
    CHECK(WaitForDetails(app, sid, [](const SessionState::Snapshot& s) {
        return s.latest_notice.has_value() && s.latest_notice->is_error;
    }));
    CHECK(app.GetDetails(sid).state == SessionStateKind::Error);
}
