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
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include <unistd.h>

#include <catch2/catch_test_macros.hpp>

#include <ftxui/dom/elements.hpp>

#include "../plugins/server/commands-core/core_commands.hpp"
#include "deadline.hpp"
#include "fakes.hpp"
#include "minimal/rpc/session_wiring.hpp"
#include "minimal/client/keybinding_dispatch.hpp"
#include "minimal/client/plugins/host_seams.hpp"
#include "minimal/client/session_client.hpp"
#include "minimal/client/state.hpp"
#include "minimal/client/view/prompt_host.hpp"
#include "minimal/client/view/tab_activity.hpp"
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
                                 {"models", {{"test-model", nlohmann::json::object()},
                                             {"other-model", nlohmann::json::object()}}}}}}}}
                  .dump());
        Write("settings.json", nlohmann::json{{"default_agent_preset", "main"},
                                              {"default_model", "main/test-model"}}.dump());
        Write("permissions.json", "{}");
        Write("agents/main.md", "---\nname: main\nmodel: main/test-model\nallowed_tools: [\"*\"]\n---\nYou help.\n");

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
            dir, config::LoadSettingsConfig(dir / "settings.json"));
        observers = std::make_unique<agent::SessionObserverRegistry>(common::DefaultLogger());
        db = std::make_unique<session::SessionDatabase>(dir / "sessions.db");
        for (auto& command : commands_core::MakeCoreCommands()) commands.Register(std::move(command));
        commands.Register(commands_core::MakeHelpCommand(commands));
        const plugins::HostRegistries registries{.tools = &tools,
                                                 .providers = &providers,
                                                 .commands = &commands,
                                                 .observers = observers.get()};
        sessions = std::make_unique<agent::SessionRegistry>(
            *presets, rpc::FinishSessionWiring, registries, *db, agent::LoopConfig{}, common::DefaultLogger(), attach_guard);
        rpc::SessionServiceImpl::Options service_options;
        service_options.presets = presets.get();  // RunSessionlessCommand dispatches against these
        service = std::make_unique<rpc::SessionServiceImpl>(
            *sessions, *db, config::SettingsConfig{}, config::PermissionsConfig{},
            common::DefaultLogger(), registries, std::move(service_options));

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

// A minimal panel view stub for the HandleGlobalKey test below: the tests
// need only spec() (placement=Panel) and a component that unconditionally
// consumes events, to prove the suggestions_open carve-out keeps it from
// ever being reached -- same shape as keybinding_dispatch_test.cpp's own
// PanelStubView, but with a REAL (event-consuming) component: that file
// never drives HandleGlobalKey itself, so its stub leaves component() null.
class ConsumingPanelStubView final : public client::plugins::View {
public:
    explicit ConsumingPanelStubView(std::string id)
        : spec_{std::move(id), "", client::plugins::ViewPlacement::Panel, ""} {}
    const client::plugins::ViewSpec& spec() const override { return spec_; }
    ftxui::Component component() override {
        return ftxui::CatchEvent(ftxui::Renderer([] { return ftxui::text(""); }),
                                 [](const ftxui::Event&) { return true; });
    }

private:
    client::plugins::ViewSpec spec_;
};

client::plugins::PluginViewEntry ConsumingPanel(const std::string& id, bool visible, bool focused) {
    static std::vector<std::unique_ptr<client::plugins::View>> store;
    store.push_back(std::make_unique<ConsumingPanelStubView>(id));
    // HandleGlobalKey dispatches through PluginViewEntry::component directly
    // (not View::component()) -- bootstrap populates that field once at
    // startup, so the test must too, or the dispatch loop calls OnEvent on a
    // null Component.
    return client::plugins::PluginViewEntry{store.back().get(), store.back()->component(), visible,
                                            focused, id, {}};
}

// The tab's SESSION id ("" while the tab is sessionless -- OpenTab mints a
// tab only, the session appears with the first submitted message).
std::string SessionIdOf(AppState& app, const std::string& tab_id) {
    for (const auto& tab : app.GetAllTabs()) {
        if (tab.tab_id == tab_id) return tab.session_id;
    }
    return "";
}

// Polls a tab's snapshot until `pred` is true or the deadline trips. AppState is
// updated from a reader thread, so tests must wait rather than assert immediately.
// Keyed by TAB id: the tab handle is the only identity a sessionless tab has.
// Generous: each wait returns as soon as its predicate holds, so the bound only
// matters for a failure -- and 5s flaked under a loaded parallel ctest (a CI
// runner, -j16 locally), where an in-process turn round trip can take seconds.
constexpr auto kWaitTimeout = std::chrono::seconds(15);

template <typename Pred>
bool WaitFor(AppState& app, const std::string& tab_id, Pred pred,
             std::chrono::milliseconds timeout = kWaitTimeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        for (const auto& tab : app.GetAllTabs()) {
            if (tab.tab_id == tab_id && pred(tab)) return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return false;
}

// Waits until the tab's session exists (the first SubmitChatTurn's
// EnsureSession round trip resolved).
bool WaitForSession(AppState& app, const std::string& tab_id,
                    std::chrono::milliseconds timeout = kWaitTimeout) {
    return WaitFor(app, tab_id, [](const AppState::TabSnapshot& t) { return t.has_session; },
                   timeout);
}

}  // namespace

TEST_CASE("OpenTab opens a sessionless tab; the session appears on the first send",
          "[app_state]") {
    // THE 0-MESSAGE-ROW FIX: OpenTab mints a TAB (a client-side container) and
    // no session -- nothing is created server-side until a message is
    // submitted, so an abandoned tab leaves no row behind.
    ClientFixture fixture;
    fixture.Script("hi");
    fixture.Start();

    AppState app(fixture.client);
    const auto tab = app.OpenTab(fixture.Options("first"));
    REQUIRE_FALSE(tab.empty());
    REQUIRE(app.HasTab(tab));

    auto tabs = app.GetAllTabs();
    REQUIRE(tabs.size() == 1);
    CHECK(tabs[0].tab_id == tab);
    CHECK(tabs[0].session_id.empty());
    CHECK_FALSE(tabs[0].has_session);
    CHECK(tabs[0].is_focused);

    // The send is what creates the session.
    app.SubmitChatTurn(tab, "hi");
    REQUIRE(WaitForSession(app, tab));
    REQUIRE_FALSE(SessionIdOf(app, tab).empty());
    tabs = app.GetAllTabs();
    CHECK(tabs[0].has_session);
}

TEST_CASE("RequestSuggestPathsForRoot answers through the real RPC for a "
          "sessionless tab's project root",
          "[app_state][mention]") {
    // The sessionless-tab scope of '@' autocomplete: the query carries the
    // project root directly (no session row exists yet), and the answer is
    // relative to that root -- the same wire contract the session scope has.
    ClientFixture fixture;
    fixture.Script("hi");
    fixture.Start();

    { std::ofstream out(fixture.dir / "work" / "README.md"); out << "y"; }
    std::filesystem::create_directories(fixture.dir / "work" / "src");

    AppState app(fixture.client);   // no tab even needed: the RPC is tab-independent

    std::promise<std::vector<std::string>> done;
    auto answered = done.get_future();
    app.RequestSuggestPathsForRoot((fixture.dir / "work").string(), "", 50,
                                   [&done](std::vector<std::string> paths) {
                                       done.set_value(std::move(paths));
                                   });
    using namespace std::chrono_literals;
    REQUIRE(answered.wait_for(5s) == std::future_status::ready);
    const auto paths = answered.get();
    REQUIRE(paths.size() == 2);
    CHECK(paths[0] == "README.md");
    CHECK(paths[1] == "src/");
}

TEST_CASE("HostFocusProvider::Detach makes Tabs()/FocusedSessionId() safe to call after",
          "[app_state][host_seams]") {
    // RunTui declares AppState's owner after the plugin registries (this
    // provider's owner), so locals unwind in the reverse order -- AppState
    // dies FIRST. A plugin poller thread still calling Tabs() during the
    // registries' own teardown used to dereference a freed AppState;
    // Detach() (called explicitly before AppState unwinds) is the fix. This
    // test can't reproduce the use-after-free directly (AppState here
    // outlives the provider, the opposite of RunTui's real ordering) -- it
    // asserts the CONTRACT Detach() provides: once it returns, the provider
    // answers its own "no focus knowledge" empty values instead of touching
    // app_ at all.
    ClientFixture fixture;
    fixture.Script("hi");
    fixture.Start();

    AppState app(fixture.client);
    const auto tab = app.OpenTab(fixture.Options("focus-me"));
    REQUIRE_FALSE(tab.empty());
    app.SetFocusedTab(tab);

    auto provider = minimal::client::plugins::MakeHostFocusProvider();
    provider->Attach(&app);
    CHECK(provider->FocusedSessionId().empty());   // sessionless tab: no session id yet
    REQUIRE(provider->Tabs().size() == 1);

    provider->Detach();
    CHECK(provider->FocusedSessionId().empty());
    CHECK(provider->Tabs().empty());
}

TEST_CASE("HostUi::RequestRedraw bumps the UI version while attached, not after Detach",
          "[app_state][host_seams]") {
    // A plugin whose data changed on its own thread asks for a frame: the
    // pump repaints only when UiVersion moves.
    ClientFixture fixture;
    fixture.Script("hi");
    fixture.Start();
    AppState app(fixture.client);

    auto provider = minimal::client::plugins::MakeHostFocusProvider();
    minimal::client::plugins::HostUi& ui = *provider;
    ui.RequestRedraw();   // not attached yet: a no-op, never a crash
    provider->Attach(&app);
    const auto before = app.UiVersion();
    ui.RequestRedraw();
    CHECK(app.UiVersion() > before);
    provider->Detach();
    const auto detached = app.UiVersion();
    ui.RequestRedraw();
    CHECK(app.UiVersion() == detached);
}

TEST_CASE("HostUi view requests queue from any thread and drain in order",
          "[app_state][host_seams]") {
    // Plugins call these from their own threads; the UI thread applies them.
    auto provider = minimal::client::plugins::MakeHostFocusProvider();
    minimal::client::plugins::HostUi& ui = *provider;
    std::thread worker([&ui] {
        ui.ShowView("a");
        ui.SetPanelVisible("b", false);
    });
    worker.join();
    ui.CloseView("a");
    const auto requests = provider->TakeViewRequests();
    REQUIRE(requests.size() == 3);
    using Op = minimal::client::plugins::ViewRequest::Op;
    CHECK(requests[0].op == Op::kShow);
    CHECK(requests[0].view_id == "a");
    CHECK(requests[1].op == Op::kSetPanelVisible);
    CHECK_FALSE(requests[1].visible);
    CHECK(requests[2].op == Op::kClose);
    CHECK(provider->TakeViewRequests().empty());   // drained
}

TEST_CASE("HasQueuedWork sees a sessionless tab's queued action before "
          "EnsureSession resolves it",
          "[app_state]") {
    // AnyWorkInFlight (keybinding_dispatch.hpp) reads SessionStateKind, which
    // stays Idle for a freshly queued sessionless tab until its create round
    // trip actually starts -- so quit's confirmation couldn't see this work
    // at all, and StopRpcQueue() DISCARDS the queue rather than draining it
    // (rpc_queue.hpp), silently dropping the message.
    ClientFixture fixture;
    fixture.Script("hi");
    fixture.Start();

    AppState app(fixture.client);
    const auto tab = app.OpenTab(fixture.Options("queued"));
    REQUIRE_FALSE(tab.empty());
    CHECK_FALSE(app.HasQueuedWork());   // nothing queued on a fresh, untouched tab

    app.SubmitChatTurn(tab, "hello");
    // The queued action is pushed synchronously (under AppState's lock)
    // before EnsureSession is even scheduled onto the RPC worker -- checked
    // immediately, with no wait, so this is not racing the worker's own
    // (separate-thread) create round trip.
    CHECK(app.HasQueuedWork());

    REQUIRE(WaitForSession(app, tab));
    CHECK_FALSE(app.HasQueuedWork());   // drained once the create resolved
}

TEST_CASE("two tabs do not cross-talk", "[app_state]") {
    ClientFixture fixture;
    fixture.Script("scripted reply");
    fixture.Start();

    AppState app(fixture.client);
    const auto tab_a = app.OpenTab(fixture.Options("a"));
    const auto tab_b = app.OpenTab(fixture.Options("b"));
    REQUIRE_FALSE(tab_a.empty());
    REQUIRE_FALSE(tab_b.empty());

    app.SubmitChatTurn(tab_a, "hello from a");
    REQUIRE(WaitForSession(app, tab_a));
    REQUIRE(WaitFor(app, tab_a, [](const AppState::TabSnapshot& t) {
        return t.state == SessionStateKind::Idle || t.state == SessionStateKind::Thinking ||
               t.state == SessionStateKind::Working;
    }));
    // Give the turn a moment to actually land before checking B was untouched --
    // WaitFor above only proves A started moving, not that the turn finished.
    std::this_thread::sleep_for(200ms);

    bool b_touched = false;
    for (const auto& tab : app.GetAllTabs()) {
        if (tab.tab_id == tab_b) b_touched = (tab.state != SessionStateKind::Idle);
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
    const auto tab = app.AttachTab(created.session_id);
    REQUIRE_FALSE(tab.empty());
    REQUIRE(app.HasTab(tab));
    CHECK(SessionIdOf(app, tab) == created.session_id);
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

    const auto tab = app.AttachTab(created.session_id);
    REQUIRE_FALSE(tab.empty());
    REQUIRE(app.HasTab(tab));

    bool resumed_focused = false;
    for (const auto& t : app.GetAllTabs()) {
        if (t.tab_id == tab) resumed_focused = t.is_focused;
    }
    CHECK_FALSE(resumed_focused);   // AttachTab alone leaves the original tab focused

    app.SetFocusedTab(tab);
    resumed_focused = false;
    for (const auto& t : app.GetAllTabs()) {
        if (t.tab_id == tab) resumed_focused = t.is_focused;
    }
    CHECK(resumed_focused);
}

TEST_CASE("RequestPaused toggles the focused session's pause state", "[app_state][pause]") {
    // The /pause // /resume guard. The state lookup must be TAB-keyed
    // (GetDetails matches tab.info.tab_id): the typed command used to feed it
    // FocusedSessionId(), whose ULID can never equal a "t<n>" tab id, so the
    // guard always read paused=false -- typed /resume on a paused session
    // answered "not paused" and never sent the unpause. This test regresses
    // on the lookup itself: a sessioned tab is attached (AttachTab), paused
    // out-of-band, and the SECOND RequestPaused must see paused=true and send
    // the resume rather than reporting AlreadyRunning.
    ClientFixture fixture;
    fixture.Script("hi");
    fixture.Start();

    const auto created = fixture.client->CreateSession(fixture.Options("pausable"));
    REQUIRE(created.status.ok());

    AppState app(fixture.client);
    const auto tab = app.AttachTab(created.session_id);
    REQUIRE_FALSE(tab.empty());
    app.SetFocusedTab(tab);
    REQUIRE(SessionIdOf(app, tab) == created.session_id);

    // Nothing to continue into yet: an idle, unpaused session.
    CHECK(app.RequestPaused(true) == AppState::PauseToggle::SentPause);
    // The confirming SessionMetadataEvent rides the wire async; every
    // emission carries `paused`, so poll the tab's snapshot for it.
    REQUIRE(WaitFor(app, tab, [](const AppState::TabSnapshot& t) { return t.paused; }));

    // The regression: on the wrong key this returned AlreadyRunning (the
    // lookup read unpaused) and the resume never reached the server.
    CHECK(app.RequestPaused(false) == AppState::PauseToggle::SentResume);
    REQUIRE(WaitFor(app, tab, [](const AppState::TabSnapshot& t) { return !t.paused; }));

    // Idempotence, now that the snapshot is live on both branches: toggling
    // to the state the session is already in sends nothing.
    CHECK(app.RequestPaused(false) == AppState::PauseToggle::AlreadyRunning);
    CHECK(app.RequestPaused(true) == AppState::PauseToggle::SentPause);
    REQUIRE(WaitFor(app, tab, [](const AppState::TabSnapshot& t) { return t.paused; }));
    CHECK(app.RequestPaused(true) == AppState::PauseToggle::AlreadyPaused);

    // A sessionless tab has nothing to pause -- and focus is what selects
    // the target, so a sessionless tab focused INSTEAD changes the answer.
    const auto bare = app.OpenTab(fixture.Options("sessionless"));
    REQUIRE_FALSE(bare.empty());
    app.SetFocusedTab(bare);
    CHECK(app.RequestPaused(true) == AppState::PauseToggle::NoSession);
    CHECK(app.RequestPaused(false) == AppState::PauseToggle::NoSession);
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
    const auto tab = app.OpenTab(fixture.Options("rewind-target"));
    REQUIRE_FALSE(tab.empty());

    app.SubmitChatTurn(tab, "first message");
    REQUIRE(WaitFor(app, tab, [](const AppState::TabSnapshot& t) {
        return t.state == SessionStateKind::Idle && t.has_session;
    }));
    app.SubmitChatTurn(tab, "second message");
    REQUIRE(WaitFor(app, tab, [](const AppState::TabSnapshot& t) {
        return t.state == SessionStateKind::Idle && t.has_session;
    }));

    // Scrollback/rewind resolution are SESSION-keyed wire paths.
    const auto sid = SessionIdOf(app, tab);
    REQUIRE_FALSE(sid.empty());

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

TEST_CASE("FindRewindTargetSeq refuses once the window is trimmed",
          "[app_state][rewind]") {
    // Regression: a trimmed window's front is not the true newest message
    // (the sliding window evicts from the NEWEST end once
    // scrollback_max_messages is exceeded by older-page loads) -- counting
    // "n user turns back" off it could silently target a turn far older
    // than the user asked for, and /rewind enacts whatever this returns.
    ClientFixture fixture;
    fixture.ScriptSteps({testing::TextResponse("first reply"), testing::TextResponse("second reply"),
                         testing::TextResponse("third reply")});
    fixture.Start();

    AppState app(fixture.client);
    // A tiny cap so three turns' worth of history (6 rows) overflows it
    // almost immediately -- applied to sessions created AFTER this call.
    app.SetUiBounds(/*scrollback_max_messages=*/3, /*tool_output_buffer_bytes=*/64 * 1024);
    const auto tab = app.OpenTab(fixture.Options("trimmed-rewind"));
    REQUIRE_FALSE(tab.empty());

    app.SubmitChatTurn(tab, "first message");
    REQUIRE(WaitForSession(app, tab));
    app.SubmitChatTurn(tab, "second message");
    app.SubmitChatTurn(tab, "third message");

    const auto sid = SessionIdOf(app, tab);
    REQUIRE_FALSE(sid.empty());

    // Settle the newest slice (capped to 3 rows by the fix under test) AND the
    // persistence of all three turns: the trim below needs the window to hold
    // the transcript's newest rows before an older-page load has anything to
    // evict. Gated only on "non-empty", this loop could pass while turn 3 is
    // still landing server-side -- the window then sits at the transcript's
    // OLDEST rows with has_earlier false, every older request is a no-op
    // (NextScrollbackCursor refuses), and the trim never fires. Under TSan's
    // slowdown that is exactly the shape this test's one flake took.
    const auto page_has_third_turn = [](const SessionState::Snapshot& details) {
        if (!details.scrollback_page || details.scrollback_page->empty()) return false;
        for (const auto& stored : *details.scrollback_page) {
            if (stored.message().role() != v1::ROLE_USER) continue;
            for (const auto& block : stored.message().content()) {
                if (block.has_text() &&
                    block.text().text().find("third message") != std::string::npos) {
                    return true;
                }
            }
        }
        return false;
    };
    const auto deadline1 = std::chrono::steady_clock::now() + 5s;
    while (!page_has_third_turn(app.GetDetails(tab)) && std::chrono::steady_clock::now() < deadline1) {
        app.RequestScrollback(sid, /*newest=*/true);
        std::this_thread::sleep_for(50ms);
    }
    REQUIRE(page_has_third_turn(app.GetDetails(tab)));
    REQUIRE_FALSE(app.GetDetails(tab).scrollback_page->empty());

    // Load older until the sliding window's cap-triggered eviction flags
    // the trim (session_state_test.cpp's INVARIANT test covers the
    // mechanism directly; this proves FindRewindTargetSeq respects it on a
    // live, AppState-owned session). The exit condition is the CONTRACT,
    // not just the flag: a newest refresh that catches the window back up
    // clears scrollback_trimmed_ by design (state.cpp's delta branch), so
    // sampling the flag alone after the loop can race a straggler refresh
    // and read false. Refuse-when-trimmed is what the loop waits for.
    const auto rewind_refused = [&] {
        const auto& details = app.GetDetails(tab);
        return details.scrollback_trimmed && !app.FindRewindTargetSeq(sid, 1).has_value();
    };
    const auto deadline2 = std::chrono::steady_clock::now() + 10s;
    while (!rewind_refused() && std::chrono::steady_clock::now() < deadline2) {
        app.RequestScrollback(sid, /*newest=*/false);
        std::this_thread::sleep_for(50ms);
    }
    REQUIRE(rewind_refused());

    CHECK_FALSE(app.FindRewindTargetSeq(sid, 1).has_value());
}

// The client's half of the rewind feature: the server emits SessionRewoundEvent
// for both verbs, and DispatchToTab must route it -- the dispatch chain used to
// have no branch, so a rewind left the discarded rows on screen and a redo
// never refetched the restored ones. Quiesces the RPC queue first so a stale
// pre-rewind page cannot reapply over the event's effect (the fetches and the
// rewind are not ordered against each other on the wire).
TEST_CASE("a rewind trims the tab's scrollback; redo clears it for a refetch",
          "[app_state][rewind]") {
    ClientFixture fixture;
    fixture.ScriptSteps({testing::TextResponse("first reply"), testing::TextResponse("second reply")});
    fixture.Start();

    AppState app(fixture.client);
    const auto tab = app.OpenTab(fixture.Options("rewound"));
    REQUIRE_FALSE(tab.empty());

    const auto rows = [&app](const std::string& tab_id) {
        return app.GetDetails(tab_id).scrollback_page->size();
    };
    const auto wait_rows = [&](const std::string& session, const std::string& tab_id,
                               size_t n) {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline) {
            app.RequestScrollback(session, /*newest=*/true);
            std::this_thread::sleep_for(50ms);
            if (rows(tab_id) >= n) return true;
        }
        return false;
    };
    // Runs after every fetch queued before it (the worker is FIFO): after this
    // returns, no fetch can race the rewind event's effect on the window.
    const auto quiesce = [&app] {
        std::atomic<bool> drained{false};
        app.RunOnRpcQueue([&drained] { drained = true; });
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!drained.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(5ms);
        }
        return drained.load();
    };

    app.SubmitChatTurn(tab, "first message");
    REQUIRE(WaitForSession(app, tab));
    const auto sid = SessionIdOf(app, tab);
    REQUIRE(wait_rows(sid, tab, 2));   // user + assistant, persisted
    app.SubmitChatTurn(tab, "second message");
    REQUIRE(wait_rows(sid, tab, 4));
    REQUIRE(quiesce());

    // REWIND to the second user turn: the server discards from target_seq
    // onward (active_last_seq = target - 1) and emits the event; the client
    // trims rows past active_last_seq instead of ignoring them.
    const auto target = app.FindRewindTargetSeq(sid, 1);   // the newest user turn
    REQUIRE(target.has_value());
    app.SendRewind(sid, target, /*redo=*/false);
    const auto trimmed = std::chrono::steady_clock::now() + 5s;
    while (rows(tab) != 2 && std::chrono::steady_clock::now() < trimmed) {
        std::this_thread::sleep_for(5ms);
    }
    REQUIRE(rows(tab) == 2);

    // REDO (truncated_to_seq == -1): the restored rows are unknown client-side
    // -- the window CLEARS (the refetch then brings all four rows back; a
    // client that never handled the event would still show the trimmed two).
    REQUIRE(quiesce());
    app.SendRewind(sid, std::nullopt, /*redo=*/true);
    const auto cleared = std::chrono::steady_clock::now() + 5s;
    while (rows(tab) != 0 && std::chrono::steady_clock::now() < cleared) {
        std::this_thread::sleep_for(5ms);
    }
    REQUIRE(rows(tab) == 0);
    REQUIRE(wait_rows(sid, tab, 4));   // the restored transcript refetches whole
}

TEST_CASE("subagent grouping and focus via OnSubagentSpawned", "[app_state]") {
    ClientFixture fixture;
    fixture.Script("hi");
    fixture.Start();

    AppState app(fixture.client);
    const auto parent_tab = app.OpenTab(fixture.Options("parent"));
    REQUIRE_FALSE(parent_tab.empty());

    // OnSubagentSpawned parents by the parent's SESSION id (the wire event
    // carries session ids), so the parent tab needs a session first.
    app.SubmitChatTurn(parent_tab, "hi");
    REQUIRE(WaitForSession(app, parent_tab));
    const auto parent_sid = SessionIdOf(app, parent_tab);
    REQUIRE_FALSE(parent_sid.empty());

    // No real subagent spawning on this branch yet -- stand in with a second,
    // independently-created real session and drive AppState's own grouping/focus
    // logic directly, which is what this test actually exercises.
    const auto created = fixture.client->CreateSession(fixture.Options("child"));
    REQUIRE(created.status.ok());

    v1::SubagentStartedEvent blocking_evt;
    blocking_evt.set_child_session_id(created.session_id);
    blocking_evt.set_blocking(true);
    app.OnSubagentSpawned(parent_sid, blocking_evt);

    auto tabs = app.GetAllTabs();
    const AppState::TabSnapshot* parent_snap = nullptr;
    const AppState::TabSnapshot* child_snap = nullptr;
    for (const auto& t : tabs) {
        if (t.tab_id == parent_tab) parent_snap = &t;
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
    const auto parent_tab = app.OpenTab(fixture.Options("parent"));
    REQUIRE_FALSE(parent_tab.empty());
    REQUIRE(app.GetAllTabs()[0].is_focused);

    app.SubmitChatTurn(parent_tab, "hi");
    REQUIRE(WaitForSession(app, parent_tab));
    const auto parent_sid = SessionIdOf(app, parent_tab);

    const auto created = fixture.client->CreateSession(fixture.Options("child"));
    REQUIRE(created.status.ok());

    v1::SubagentStartedEvent nonblocking_evt;
    nonblocking_evt.set_child_session_id(created.session_id);
    nonblocking_evt.set_blocking(false);
    app.OnSubagentSpawned(parent_sid, nonblocking_evt);

    for (const auto& t : app.GetAllTabs()) {
        if (t.tab_id == parent_tab) CHECK(t.is_focused);
        if (t.session_id == created.session_id) CHECK_FALSE(t.is_focused);
    }
}

TEST_CASE("CloseTab removes the tab and reassigns focus", "[app_state]") {
    ClientFixture fixture;
    fixture.Script("hi");
    fixture.Start();

    AppState app(fixture.client);
    const auto tab_a = app.OpenTab(fixture.Options("a"));
    const auto tab_b = app.OpenTab(fixture.Options("b"));
    REQUIRE_FALSE(tab_a.empty());
    REQUIRE_FALSE(tab_b.empty());
    app.SetFocusedTab(tab_b);

    app.CloseTab(tab_b);
    REQUIRE_FALSE(app.HasTab(tab_b));
    auto tabs = app.GetAllTabs();
    REQUIRE(tabs.size() == 1);
    CHECK(tabs[0].tab_id == tab_a);
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
    const auto tab_a = app.OpenTab(fixture.Options("a"));
    const auto tab_b = app.OpenTab(fixture.Options("b"));
    const auto tab_c = app.OpenTab(fixture.Options("c"));
    const auto tab_d = app.OpenTab(fixture.Options("d"));
    REQUIRE_FALSE(tab_a.empty());
    REQUIRE_FALSE(tab_b.empty());
    REQUIRE_FALSE(tab_c.empty());
    REQUIRE_FALSE(tab_d.empty());
    app.SetFocusedTab(tab_b);

    app.CloseTab(tab_b);   // the middle tab, focused
    REQUIRE_FALSE(app.HasTab(tab_b));
    auto tabs = app.GetAllTabs();
    REQUIRE(tabs.size() == 3);
    CHECK(tabs[0].tab_id == tab_a);
    CHECK(tabs[1].tab_id == tab_c);
    CHECK(tabs[2].tab_id == tab_d);
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
    const auto tab_a = app.OpenTab(fixture.Options("a"));
    const auto tab_b = app.OpenTab(fixture.Options("b"));
    const auto tab_c = app.OpenTab(fixture.Options("c"));
    REQUIRE_FALSE(tab_a.empty());
    REQUIRE_FALSE(tab_b.empty());
    REQUIRE_FALSE(tab_c.empty());
    app.SetFocusedTab(tab_c);

    app.CloseTab(tab_c);   // the last tab, focused -- erase() returns end()
    REQUIRE_FALSE(app.HasTab(tab_c));
    auto tabs = app.GetAllTabs();
    REQUIRE(tabs.size() == 2);
    CHECK(tabs[1].tab_id == tab_b);
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
    CHECK(snap.scrollback_page->size() == static_cast<size_t>(page.response.messages_size()));
    CHECK(snap.last_loaded_seq == page.response.messages(page.response.messages_size() - 1).seq());
}

// Polls a tab's DEEP snapshot until `pred` is true or the deadline trips (the
// pending-modal data rides GetDetails, not the light tab list). GetDetails is
// TAB-keyed (a sessionless tab has no session id to key by).
template <typename Pred>
bool WaitForDetails(AppState& app, const std::string& tab_id, Pred pred,
                    std::chrono::milliseconds timeout = kWaitTimeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred(app.GetDetails(tab_id))) return true;
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
bool WaitForAttached(AppState& app, const std::string& tab_id,
                     std::chrono::milliseconds timeout = kWaitTimeout) {
    return WaitForDetails(app, tab_id, [](const SessionState::Snapshot& s) {
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
    const auto tab = app.OpenTab(fixture.Options("solo"));
    REQUIRE_FALSE(tab.empty());
    REQUIRE(app.HasTab(tab));

    app.SubmitChatTurn(tab, "hi");
    REQUIRE(WaitForSession(app, tab));
    REQUIRE(WaitForAttached(app, tab));
    fixture.sessions->Terminate(SessionIdOf(app, tab));   // server-side; the tab is untouched

    // The wire SessionEndedEvent (published before the server closes the
    // subscriber queues) routes to OnSessionEnded -- the stream itself stays
    // open, so the tab must not depend on a stream close to reach Done.
    REQUIRE(WaitFor(app, tab, [](const AppState::TabSnapshot& t) {
        return t.state == SessionStateKind::Done;
    }));
    CHECK(app.HasTab(tab));   // the tab itself is never removed automatically
}

TEST_CASE("EndTabSessions TERMINATE destroys the session server-side", "[app_state]") {
    // The close confirmation's "close anyway": the ONLY client path that
    // ends a session on the wire (EndSession{TERMINATE}). The stream ends,
    // the live session is torn down with everything in flight -- unlike
    // CloseTab's bare drop, which the server reads as a DETACH.
    ClientFixture fixture;
    fixture.Script("hi");
    fixture.Start();

    AppState app(fixture.client);
    const auto tab = app.OpenTab(fixture.Options("kill-me"));
    REQUIRE_FALSE(tab.empty());
    app.SubmitChatTurn(tab, "hi");
    REQUIRE(WaitForSession(app, tab));
    REQUIRE(WaitForAttached(app, tab));
    const auto sid = SessionIdOf(app, tab);

    app.EndTabSessions({tab}, minimal::client::EndSessionMode::kTerminate);
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (fixture.sessions->Find(sid) != nullptr &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    CHECK(fixture.sessions->Find(sid) == nullptr);
}

TEST_CASE("EndTabSessions DETACH leaves the session running and the tab closable",
          "[app_state]") {
    // The close confirmation's "detach" and /detach-session: the stream ends
    // with an explicit EndSession{DETACH}; the session keeps running
    // server-side (reattachable while it lives), and the tab erase that
    // follows lands on the already-latched stream as a no-op.
    ClientFixture fixture;
    fixture.Script("hi");
    fixture.Start();

    AppState app(fixture.client);
    const auto tab = app.OpenTab(fixture.Options("detach-me"));
    REQUIRE_FALSE(tab.empty());
    app.SubmitChatTurn(tab, "hi");
    REQUIRE(WaitForSession(app, tab));
    REQUIRE(WaitForAttached(app, tab));
    const auto sid = SessionIdOf(app, tab);

    app.EndTabSessions({tab}, minimal::client::EndSessionMode::kDetach);
    app.CloseTab(tab);   // erases the tab; the detached stream is already ended
    REQUIRE_FALSE(app.HasTab(tab));

    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (std::chrono::steady_clock::now() < deadline) {
        // Give the server's read loop a moment to observe the DETACH -- the
        // assertion is that it NEVER erases the session for one.
        if (fixture.sessions->Find(sid) == nullptr) break;
        std::this_thread::sleep_for(10ms);
    }
    REQUIRE(fixture.sessions->Find(sid) != nullptr);
    CHECK_FALSE(fixture.sessions->Find(sid)->terminated());
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
        const auto tab = app.OpenTab(fixture.Options("close-race-" + std::to_string(i)));
        REQUIRE_FALSE(tab.empty());
        app.SubmitChatTurn(tab, "hi");   // stream events flowing on the reader thread
        app.CloseTab(tab);               // erase + join while dispatch/ensure in flight
        REQUIRE_FALSE(app.HasTab(tab));
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
    const auto tab = app.OpenTab(fixture.Options("cancel-me"));
    REQUIRE_FALSE(tab.empty());

    app.SubmitChatTurn(tab, "build it");
    REQUIRE(WaitForDetails(app, tab,
                           [](const SessionState::Snapshot& s) {
                               return s.pending_permission.has_value();
                           }));
    CHECK(app.GetDetails(tab).state == SessionStateKind::Waiting);

    // THE VERB: the server ends the turn with STOP_REASON_CANCELLED and
    // answers the parked prompt with a denial (pending.CancelAll). Before
    // this was wired the TUI had no interrupt at all. SendCancel is
    // SESSION-keyed (a busy tab is necessarily sessioned).
    app.SendCancel(SessionIdOf(app, tab));
    CHECK(WaitFor(app, tab, [](const AppState::TabSnapshot& t) {
        return t.state == SessionStateKind::Idle;
    }));
    CHECK_FALSE(app.GetDetails(tab).cancel_requested);   // retired by the turn end
    CHECK_FALSE(app.GetDetails(tab).pending_permission.has_value());

    // A cancel must not poison the session: turn 2 runs to completion.
    app.SubmitChatTurn(tab, "try again");
    CHECK(WaitFor(app, tab, [](const AppState::TabSnapshot& t) {
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
    const auto tab = app.OpenTab(fixture.Options("confirm-me"));
    REQUIRE_FALSE(tab.empty());

    // SubmitCommand on a sessionless tab runs EnsureSession first, then the
    // command on the created session -- the same path a sessioned tab takes
    // once the create resolves.
    app.SubmitCommand(tab, "/clear");
    REQUIRE(WaitFor(app, tab, [](const AppState::TabSnapshot& t) {
        return t.state == SessionStateKind::Waiting;
    }));

    const auto pending = app.GetDetails(tab).pending_confirmation;
    REQUIRE(pending.has_value());
    REQUIRE(pending->options.size() == 2);   // the dispatcher's {"Yes", "No"}

    app.AnswerConfirmation(SessionIdOf(app, tab), pending->request_id, "Yes");

    // THE WEDGE FIX: no tool events or message stops follow a command
    // confirmation, so the state must re-derive at the answer -- not sit in
    // Waiting until some unrelated event happens to arrive.
    CHECK(WaitFor(app, tab, [](const AppState::TabSnapshot& t) {
        return t.state == SessionStateKind::Idle;
    }));
    // The command actually ran: its result comes back as the notice.
    CHECK(WaitForDetails(app, tab, [](const SessionState::Snapshot& s) {
        return s.latest_notice.has_value();
    }));
}

TEST_CASE("AnswerPermission unwedges the tab immediately, before the tool's own "
          "started event",
          "[app_state][permission]") {
    // Mirrors "a wire confirmation is answered and unwedges the tab": the
    // server collects every permission of a round BEFORE starting any tool,
    // so nothing guarantees a ToolExecutionEvent follows an individual
    // permission answer right away -- AnswerPermission must re-derive
    // state_ itself (OnPendingResolved), the same as AnswerConfirmation and
    // AnswerAsk already did, or the tab stays Waiting (with the attention
    // glyph) for the whole tool run that follows.
    ClientFixture fixture;
    fixture.tools.Register(
        std::make_unique<testing::FakeTool>("bash", tools::ToolCategory::Execute, "ran it"));
    fixture.ScriptSteps({testing::ToolUseResponse("call-1", "bash", {{"command", "make"}}),
                         testing::TextResponse("done")});
    fixture.Write("permissions.json", nlohmann::json{{"fallback", {{"execute", "ask"}}}}.dump());
    fixture.Start();

    AppState app(fixture.client);
    const auto tab = app.OpenTab(fixture.Options("permission-me"));
    REQUIRE_FALSE(tab.empty());

    app.SubmitChatTurn(tab, "build it");
    REQUIRE(WaitForDetails(app, tab, [](const SessionState::Snapshot& s) {
        return s.pending_permission.has_value();
    }));
    const auto pending = app.GetDetails(tab).pending_permission;
    REQUIRE(pending.has_value());

    app.AnswerPermission(SessionIdOf(app, tab), pending->request_id,
                         v1::PERMISSION_DECISION_ALLOW, {});

    // THE FIX: state must move off Waiting on the answer itself, not wait
    // for the tool's own started event.
    CHECK(WaitFor(app, tab, [](const AppState::TabSnapshot& t) {
        return t.state != SessionStateKind::Waiting;
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
    const auto tab = app.OpenTab(fixture.Options("resume-me"));
    REQUIRE_FALSE(tab.empty());

    // A bare state-Idle wait passes instantly (the tab is Idle BEFORE the
    // turn's first event lands), so turn completion is polled through the
    // transcript itself: the turn is over exactly when its rows are durable.
    // RequestScrollback is SESSION-keyed; GetDetails is TAB-keyed -- hence
    // the pair of ids.
    const auto wait_rows = [&app](const std::string& tab, const std::string& session,
                                  size_t n) {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline) {
            app.RequestScrollback(session, /*newest=*/true);
            std::this_thread::sleep_for(50ms);
            if (app.GetDetails(tab).scrollback_page->size() >= n) return true;
        }
        return false;
    };

    app.SubmitChatTurn(tab, "first message");
    REQUIRE(WaitForSession(app, tab));
    const auto sid = SessionIdOf(app, tab);
    REQUIRE(wait_rows(tab, sid, 2));   // user + assistant, persisted

    // TERMINATE (not detach): the transcript stays in SQLite, but the LIVE
    // session must be gone -- ResumeSession refuses an already-open session.
    // CloseTab drops the client side; the registry drop is polled.
    app.CloseTab(tab);
    fixture.sessions->Terminate(sid);
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (fixture.sessions->Find(sid) != nullptr &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    REQUIRE(fixture.sessions->Find(sid) == nullptr);

    // ResumeTab returns the NEW tab's tab id; the session id underneath is
    // the same stored one.
    const auto resumed = app.ResumeTab(sid);
    REQUIRE_FALSE(resumed.empty());
    REQUIRE(app.HasTab(resumed));
    REQUIRE(SessionIdOf(app, resumed) == sid);
    CHECK(app.GetAllTabs()[0].is_focused);   // the app was empty: focus-if-empty

    // THE BUG: a picked stored session must be LIVE, not a dead tab that
    // renders history but drops input.
    app.SubmitChatTurn(resumed, "second message");
    REQUIRE(wait_rows(resumed, sid, 4));   // the resumed turn completed ON TOP of the stored transcript

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

TEST_CASE("ForkSession opens the fork as a focused tab and runs the instruction there",
          "[app_state][fork]") {
    ClientFixture fixture;
    fixture.ScriptTwo("first reply", "fork reply");
    fixture.Start();

    AppState app(fixture.client);
    const auto tab = app.OpenTab(fixture.Options("fork-me"));
    app.SubmitChatTurn(tab, "first message");
    REQUIRE(WaitForSession(app, tab));
    const auto sid = SessionIdOf(app, tab);
    const auto wait_messages = [&fixture](const std::string& session, int n) {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto page = fixture.client->GetMessages(session, std::nullopt, 10);
            if (page.status.ok() && page.response.messages_size() >= n) return true;
            std::this_thread::sleep_for(50ms);
        }
        return false;
    };
    REQUIRE(wait_messages(sid, 2));

    app.ForkSession(tab, sid, "do it differently");

    // The fork lands as a second, focused tab on a NEW session.
    std::string fork_tab, fork_sid;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (fork_tab.empty() && std::chrono::steady_clock::now() < deadline) {
        for (const auto& info : app.GetAllTabs()) {
            if (info.tab_id != tab && info.is_focused && !info.session_id.empty()) {
                fork_tab = info.tab_id;
                fork_sid = info.session_id;
            }
        }
        std::this_thread::sleep_for(10ms);
    }
    REQUIRE_FALSE(fork_tab.empty());
    CHECK(fork_sid != sid);
    // Named after its source ("fork-me"), not after the instruction.
    const auto fork_row = fixture.client->GetSession(fork_sid);
    REQUIRE(fork_row.status.ok());
    CHECK(fork_row.summary.name() == "fork of fork-me");

    // The fork carries the copied turn plus the instruction's own turn; the
    // source is untouched.
    REQUIRE(wait_messages(fork_sid, 4));
    const auto page = fixture.client->GetMessages(fork_sid, std::nullopt, 10);
    REQUIRE(page.response.messages_size() == 4);
    CHECK(page.response.messages(1).message().content(0).text().text() == "do it differently");
    CHECK(page.response.messages(0).message().content(0).text().text() == "fork reply");
    CHECK(fixture.client->GetMessages(sid, std::nullopt, 10).response.messages_size() == 2);
}

TEST_CASE("ForkSession of an unknown session notices on the source tab", "[app_state][fork]") {
    ClientFixture fixture;
    fixture.Script("hi");
    fixture.Start();

    AppState app(fixture.client);
    const auto tab = app.OpenTab(fixture.Options("source"));
    app.ForkSession(tab, "01ARZ3NDEKTSV4RRFFQ69G5FAV", "");
    CHECK(WaitForDetails(app, tab, [](const SessionState::Snapshot& s) {
        return s.latest_notice.has_value() &&
               s.latest_notice->message.find("/fork failed") != std::string::npos;
    }));
    CHECK(app.GetAllTabs().size() == 1);
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
    const auto tab = app.ResumeTab(ghost);
    REQUIRE_FALSE(tab.empty());
    REQUIRE(app.HasTab(tab));
    CHECK(app.GetAllTabs()[0].is_focused);
    CHECK(WaitForDetails(app, tab, [](const SessionState::Snapshot& s) {
        return s.latest_notice.has_value() && s.latest_notice->is_error;
    }));
    CHECK(app.GetDetails(tab).state == SessionStateKind::Error);

    // And sends on that stream-less tab refuse VISIBLY, not silently. (The
    // refused tab's session_id IS the ghost id -- ResumeTab registers it --
    // which is what the session-keyed sender resolves.)
    app.SendChatTurn(ghost, "hello?");
    CHECK(WaitForDetails(app, tab, [](const SessionState::Snapshot& s) {
        return s.latest_notice.has_value() &&
               s.latest_notice->message.find("cannot send") != std::string::npos;
    }));
}

TEST_CASE("OpenTranscriptTab opens a read-only view of a finished session's transcript",
          "[app_state][transcript]") {
    // A finished subagent's transcript: the session row and its messages live
    // in SQLite, but the server refuses a resume ("its life was one turn").
    // The transcript tab must render from GetMessages WITHOUT creating a
    // session -- and must never let a submission mint one over itself.
    ClientFixture fixture;
    fixture.Script("first reply");
    fixture.Start();

    AppState app(fixture.client);
    const auto tab = app.OpenTab(fixture.Options("transcript-me"));
    REQUIRE_FALSE(tab.empty());
    app.SubmitChatTurn(tab, "first message");
    REQUIRE(WaitForSession(app, tab));
    const auto sid = SessionIdOf(app, tab);

    // The transcript is durable (user + assistant rows) before the teardown.
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        app.RequestScrollback(sid, /*newest=*/true);
        std::this_thread::sleep_for(50ms);
        if (app.GetDetails(tab).scrollback_page->size() >= 2) break;
    }
    REQUIRE(app.GetDetails(tab).scrollback_page->size() >= 2);

    app.CloseTab(tab);
    fixture.sessions->Terminate(sid);
    const auto gone = std::chrono::steady_clock::now() + 5s;
    while (fixture.sessions->Find(sid) != nullptr && std::chrono::steady_clock::now() < gone) {
        std::this_thread::sleep_for(10ms);
    }
    REQUIRE(fixture.sessions->Find(sid) == nullptr);

    // Open the read-only view: a real session id, NO stream.
    const auto view = app.OpenTranscriptTab(sid);
    REQUIRE_FALSE(view.empty());
    REQUIRE(app.HasTab(view));
    for (const auto& t : app.GetAllTabs()) {
        if (t.tab_id == view) {
            CHECK(t.session_id == sid);
            CHECK(t.view_only);
            // Stream-derived, deliberately: has_session means "can send on
            // this tab", and a view-only tab cannot.
            CHECK_FALSE(t.has_session);
        }
    }

    // The transcript renders through the ordinary scrollback machine -- the
    // unary GetMessages path needs no stream at all.
    const auto rows_deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < rows_deadline) {
        app.RequestScrollback(sid, /*newest=*/true);
        std::this_thread::sleep_for(50ms);
        if (app.GetDetails(view).scrollback_page->size() >= 2) break;
    }
    REQUIRE(app.GetDetails(view).scrollback_page->size() >= 2);

    // The stored row's name seeds the label (GetSession on the RPC worker --
    // this tab receives no metadata events, so that fetch is the only source).
    CHECK(WaitFor(app, view, [](const AppState::TabSnapshot& t) {
        return t.display_name == "transcript-me";
    }));

    // Submissions are refused VISIBLY -- the failure mode this tab's guard
    // exists to prevent is EnsureSession minting a fresh session and
    // repointing the tab over the transcript being viewed.
    app.SubmitChatTurn(view, "hello?");
    CHECK(WaitForDetails(app, view, [](const SessionState::Snapshot& s) {
        return s.latest_notice.has_value() &&
               s.latest_notice->message.find("read-only") != std::string::npos;
    }));
    app.SubmitCommand(view, "/rename nope");
    CHECK(WaitForDetails(app, view, [](const SessionState::Snapshot& s) {
        return s.latest_notice.has_value() &&
               s.latest_notice->message.find("read-only") != std::string::npos;
    }));

    // Nothing was minted and the transcript is untouched.
    REQUIRE(app.GetAllTabs().size() == 1);
    CHECK(SessionIdOf(app, view) == sid);
    const auto page = fixture.client->GetMessages(sid, std::nullopt, 10);
    REQUIRE(page.status.ok());
    CHECK(page.response.messages_size() == 2);

    // One tab per session: a second open is a no-op (the caller focuses the
    // existing tab).
    CHECK(app.OpenTranscriptTab(sid).empty());
}

TEST_CASE("ResumeTab on an already-open tab focuses it instead of duplicating",
          "[app_state][resume]") {
    ClientFixture fixture;
    fixture.Script("hi");
    fixture.Start();

    AppState app(fixture.client);
    const auto tab_a = app.OpenTab(fixture.Options("a"));
    const auto tab_b = app.OpenTab(fixture.Options("b"));
    REQUIRE_FALSE(tab_a.empty());
    REQUIRE_FALSE(tab_b.empty());
    app.SetFocusedTab(tab_b);

    // Give tab a a real session -- ResumeTab is SESSION-keyed.
    app.SubmitChatTurn(tab_a, "hi");
    REQUIRE(WaitForSession(app, tab_a));
    const auto sid_a = SessionIdOf(app, tab_a);

    // The old picker path silently no-opped this pick (AttachTab refused the
    // duplicate and returned ""); the user's pick must at least land focus.
    // (Focus comes from the picker's own SetFocusedTab after ResumeTab.)
    REQUIRE(app.ResumeTab(sid_a) == tab_a);
    REQUIRE(app.GetAllTabs().size() == 2);
    app.SetFocusedTab(tab_a);
    for (const auto& t : app.GetAllTabs()) {
        if (t.tab_id == tab_a) CHECK(t.is_focused);
        if (t.tab_id == tab_b) CHECK_FALSE(t.is_focused);
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
    const auto tab = app.OpenTab(fixture.Options("dies"));
    REQUIRE_FALSE(tab.empty());
    app.SubmitChatTurn(tab, "go");
    REQUIRE(WaitForSession(app, tab));
    const auto sid = SessionIdOf(app, tab);
    REQUIRE(WaitForAttached(app, tab));   // terminate an ATTACHED session, not a refused bidi

    // Complete one turn (durable rows prove completion; a bare state-Idle
    // wait passes instantly -- the tab is Idle before the turn starts). The
    // fetch is re-issued per retry: nothing else drives scrollback refreshes
    // in a headless test.
    bool turn_done = false;
    const auto turn_deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < turn_deadline) {
        app.RequestScrollback(sid, /*newest=*/true);
        std::this_thread::sleep_for(50ms);
        if (app.GetDetails(tab).scrollback_page->size() >= 2) {
            turn_done = true;
            break;
        }
    }
    REQUIRE(turn_done);

    fixture.sessions->Terminate(sid);
    REQUIRE(WaitFor(app, tab, [](const AppState::TabSnapshot& t) {
        return t.state == SessionStateKind::Done;
    }));

    app.SendChatTurn(sid, "anyone there?");
    CHECK(WaitForDetails(app, tab, [](const SessionState::Snapshot& s) {
        return s.latest_notice.has_value() && s.latest_notice->is_error;
    }));
    CHECK(app.GetDetails(tab).state == SessionStateKind::Error);
}

// ---------------------------------------------------------------------------
// Sessionless-tab behavior beyond the basics: a sessionless /-command, the
// /agent //model staging, and the single-flight create under stacked sends.
// ---------------------------------------------------------------------------

TEST_CASE("a sessionless command runs with no session created", "[app_state][sessionless]") {
    // The heart of the junk-row fix: /help typed on a sessionless tab is
    // dispatched over RunSessionlessCommand (no session, no stream, no row),
    // and the tab stays sessionless afterwards.
    ClientFixture fixture;
    fixture.Script("hi");
    fixture.Start();

    AppState app(fixture.client);
    const auto tab = app.OpenTab(fixture.Options("cmd"));
    REQUIRE(app.GetAllTabs()[0].has_session == false);

    app.RunSessionlessCommand(tab, "/help");
    REQUIRE(WaitForDetails(app, tab, [](const SessionState::Snapshot& s) {
        return s.latest_notice.has_value() &&
               s.latest_notice->message.find("/compact") != std::string::npos;
    }));

    // The registry still holds ZERO sessions: nothing was minted to run a
    // listing command.
    CHECK(fixture.sessions->size() == 0);
    CHECK(app.GetAllTabs()[0].has_session == false);
}

TEST_CASE("staged /agent rides the eventual CreateSession", "[app_state][sessionless]") {
    ClientFixture fixture;
    // A SECOND preset distinct from the default, so "the staged choice reached
    // the session" is provable from the metadata event (the server default is
    // also "main", which would make the assertion vacuous).
    fixture.Write("agents/alt.md",
                  "---\nname: alt\nmodel: main/test-model\nallowed_tools: [\"*\"]\n---\nAlt.\n");
    fixture.Script("hi");
    fixture.Start();

    AppState app(fixture.client);
    // NO preset in the create-options: what the session runs is whatever was
    // staged before the first message.
    SessionClient::CreateOptions opts;
    opts.project_root = (fixture.dir / "work").string();
    const auto tab = app.OpenTab(opts);
    REQUIRE_FALSE(tab.empty());

    // Visible notice + immediate display seeding. The choice lands in the
    // DESIRED layer (SessionState::desired_agent_preset -- the status bar
    // renders it immediately); the EFFECTIVE metadata field stays empty
    // until the wire metadata event proves what the session actually runs.
    // TabNotice is what the TUI's submit path pairs with the staging call,
    // so mirror it here.
    REQUIRE(app.StageAgentPreset(tab, "alt"));
    app.TabNotice(tab, "agent 'alt' will be used on your next message");
    REQUIRE(WaitForDetails(app, tab, [](const SessionState::Snapshot& s) {
        return s.latest_notice.has_value();
    }));
    CHECK(app.GetDetails(tab).desired_agent_preset == "alt");

    app.SubmitChatTurn(tab, "hi");
    REQUIRE(WaitForSession(app, tab));
    REQUIRE(WaitForAttached(app, tab));
    // THE POINT: the created session runs the STAGED preset.
    CHECK(app.GetDetails(tab).agent_preset == "alt");
}

// THE STAGED-MODEL FIX: Stream::Open's create branch set project_root/preset/
// name but never model -- a /model choice staged on a sessionless tab was
// silently discarded by the create. The metadata event echoes the session's
// resolved model reference, which is how the assertion reads it back.
TEST_CASE("staged /model rides the eventual CreateSession", "[app_state][sessionless]") {
    ClientFixture fixture;
    fixture.Script("hi");
    fixture.Start();

    AppState app(fixture.client);
    // NO model in the create-options: what the session runs is whatever was
    // staged before the first message. ("main/other-model" exists in the
    // fixture's providers.json but is NOT the preset's own -- the assertion
    // would be vacuous against the default.) The preset comes from
    // fixture.Options like every sibling test: the fixture's service is built
    // with a default SettingsConfig, so there is no settings-level default to
    // fall back on.
    const auto tab = app.OpenTab(fixture.Options("modelled"));
    REQUIRE_FALSE(tab.empty());

    REQUIRE(app.StageModel(tab, "main/other-model"));
    app.SubmitChatTurn(tab, "hi");
    REQUIRE(WaitForSession(app, tab));
    REQUIRE(WaitForAttached(app, tab));
    // THE POINT: the created session runs the STAGED model.
    CHECK(app.GetDetails(tab).model == "main/other-model");
}

// THE DESIRED-STATE CONVERGENCE: on a sessioned tab, /agent (typed or
// picker-selected) no longer sends a command at all -- it sets the tab's
// desired state, shown immediately in the snapshot, and the ACTUAL switch
// happens at the next turn start where SubmitChatTurn submits "/agent <name>"
// ahead of the turn. Selection never touches a live session directly: the
// user can swap presets fifteen times between turns and the session sees
// only the final value.
TEST_CASE("a sessioned /agent selection converges at the next turn start, not before",
          "[app_state][desired]") {
    ClientFixture fixture;
    // A SECOND preset distinct from the default ("main"), so the switch is
    // provable from the metadata event.
    fixture.Write("agents/alt.md",
                  "---\nname: alt\nmodel: main/test-model\nallowed_tools: [\"*\"]\n---\nAlt.\n");
    fixture.ScriptTwo("first reply", "second reply");
    fixture.Start();

    AppState app(fixture.client);
    const auto tab = app.OpenTab(fixture.Options("converge"));
    REQUIRE_FALSE(tab.empty());

    // Turn one creates the session on the DEFAULT preset.
    app.SubmitChatTurn(tab, "first");
    REQUIRE(WaitForSession(app, tab));
    REQUIRE(WaitForAttached(app, tab));
    REQUIRE(app.GetDetails(tab).agent_preset == "main");

    // The selection is DESIRED-ONLY: no command, no metadata change -- the
    // effective preset stays "main" while the snapshot carries the choice.
    REQUIRE(app.StageAgentPreset(tab, "alt"));
    CHECK(app.GetDetails(tab).agent_preset == "main");
    CHECK(app.GetDetails(tab).desired_agent_preset == "alt");

    // Turn two converges: the /agent command runs ahead of the turn, and the
    // metadata event confirms the session now RUNS the desired preset.
    app.SubmitChatTurn(tab, "second");
    REQUIRE(WaitForDetails(app, tab, [](const SessionState::Snapshot& s) {
        return s.agent_preset == "alt";
    }));
    // Sticky: the user's choice stands after convergence (equal to effective
    // now, so ConvergenceCommands has nothing more to send).
    CHECK(app.GetDetails(tab).desired_agent_preset == "alt");
}

// THE STARTUP-FLOW REGRESSION: a fresh client has ZERO tabs; a bare /agent
// (or /model) opens its picker before any tab exists, and the selection must
// land on the tab the first message will create -- not be silently dropped
// because FocusedTabId() is empty (the session then started on the configured
// default preset instead). This exercises the data path that lands the
// staged PAIR (agent + model, as the pickers stage them) on that tab.
TEST_CASE("agent and model staged on the first tab both reach the created session",
          "[app_state][desired]") {
    ClientFixture fixture;
    fixture.Write("agents/alt.md",
                  "---\nname: alt\nmodel: main/test-model\nallowed_tools: [\"*\"]\n---\nAlt.\n");
    fixture.Script("hi");
    fixture.Start();

    AppState app(fixture.client);
    // The empty-startup sequence: no tab exists until one is created on
    // demand (what focused_or_new_tab does for the pickers).
    REQUIRE(app.FocusedTabId().empty());
    const auto tab = app.OpenTab(fixture.Options("pair"));
    REQUIRE(app.FocusedTabId() == tab);   // OpenTab focuses an empty app's first tab

    REQUIRE(app.StageAgentPreset(tab, "alt"));
    REQUIRE(app.StageModel(tab, "main/other-model"));

    app.SubmitChatTurn(tab, "hi");
    REQUIRE(WaitForSession(app, tab));
    REQUIRE(WaitForAttached(app, tab));
    // THE POINT: the session was BORN on both staged choices -- the agent
    // AND the model (the /agent-then-/model ordering keeps the model from
    // being re-resolved away by the preset switch).
    CHECK(app.GetDetails(tab).agent_preset == "alt");
    CHECK(app.GetDetails(tab).model == "main/other-model");
}

TEST_CASE("sends stacked before the create resolves land on ONE session",
          "[app_state][sessionless]") {
    // The single-flight guarantee the TUI leans on: typing a message and
    // immediately another (or a burst of queued submits) must produce ONE
    // CreateSession, with both turns on it -- not two sessions and not a
    // dropped message.
    ClientFixture fixture;
    fixture.ScriptTwo("first reply", "second reply");
    fixture.Start();

    AppState app(fixture.client);
    const auto tab = app.OpenTab(fixture.Options("stacked"));
    app.SubmitChatTurn(tab, "first");
    app.SubmitChatTurn(tab, "second");
    REQUIRE(WaitForSession(app, tab));
    const auto sid = SessionIdOf(app, tab);

    // Both turns completed on the created session (4 durable rows).
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    bool settled = false;
    while (std::chrono::steady_clock::now() < deadline) {
        app.RequestScrollback(sid, /*newest=*/true);
        std::this_thread::sleep_for(50ms);
        if (app.GetDetails(tab).scrollback_page->size() >= 4) {
            settled = true;
            break;
        }
    }
    if (!settled) {
        // Stall forensics: the client window vs the SERVER-side durable
        // transcript. If the server also holds < 4 rows, the second turn
        // never completed (a dropped send -- product bug); if the server
        // has 4 but the window lags, the fetch/apply path stalled.
        const auto snap = app.GetDetails(tab);
        std::cerr << "[stall-diag] client page=" << snap.scrollback_page->size()
                  << " state=" << static_cast<int>(snap.state)
                  << " turn_in_flight=" << snap.turn_in_flight
                  << " turn_seq=" << snap.turn_seq
                  << " has_streaming=" << snap.has_streaming_text
                  << " pending_user=" << snap.pending_user_messages.size()
                  << " transcript_version=" << snap.transcript_version
                  << " last_loaded_seq=" << snap.last_loaded_seq
                  << " has_earlier=" << snap.has_earlier_messages;
        if (snap.latest_notice)
            std::cerr << " notice='" << snap.latest_notice->message << "'";
        std::cerr << "\n[stall-diag] client seqs:";
        for (const auto& row : *snap.scrollback_page) std::cerr << ' ' << row.seq();
        std::cerr << "\n";
        const auto server = fixture.db->GetMessagesPage(sid, std::nullopt, 100);
        std::cerr << "[stall-diag] server rows=" << server.messages.size() << " seqs:";
        for (const auto& row : server.messages) {
            std::cerr << ' ' << row.seq << ':'
                      << (row.message.role == llm::Role::User ? "U'" : "A'")
                      << llm::TextOf(row.message.content) << "'";
        }
        std::cerr << std::endl;
    }
    REQUIRE(settled);
    CHECK(fixture.sessions->size() == 1);
}

TEST_CASE("HandleGlobalKey: an open suggestion list gets Escape before "
          "turn-cancel claims it",
          "[app_state][keybinding_dispatch]") {
    // Regression: HandleGlobalKey sees every event before the prompt editor
    // does, and used to claim a busy tab's Escape for turn.cancel
    // unconditionally -- the editor's own Escape-closes-the-suggestion-list
    // handling (prompt_editor.cpp) never got a turn. This is the first
    // direct test of HandleGlobalKey itself (previously untested -- see
    // .claude/client-review/synthesis.md's test-harness-gaps note); AppState
    // has no seam to fake around, hence the e2e-weight ClientFixture.
    ClientFixture fixture;
    fixture.Script("hi");
    fixture.Start();

    AppState app(fixture.client);
    const auto tab = app.OpenTab(fixture.Options("first"));

    const auto keys = minimal::client::Keybindings::Parse(R"({"turn.cancel": "escape"})");
    const auto keymap = minimal::client::ResolvedKeymap::Build(keys);

    bool show_plugin_modal = false;
    std::vector<minimal::client::plugins::PluginViewEntry> plugin_views;
    std::shared_ptr<ftxui::Component> plugin_modal_child;
    bool suggestions_open_value = true;
    bool quit_requested = false;

    minimal::client::KeyContext ctx{
        app, keymap, tab,
        show_plugin_modal,
        plugin_views, plugin_modal_child,
        [] {}, [] {}, [] {},
        [&] { quit_requested = true; },
        [](const std::string&) {},
        [](std::vector<std::string>) {},
        {}, nullptr, nullptr,   // the chat surface's actions and mouse: none here
        // The editor claims keys while its list is open (PromptEditor::Claims).
        [&](const ftxui::Event&) { return suggestions_open_value; }};

    // Suggestions open: Escape must NOT be consumed here -- it falls through
    // to the editor's own CatchEvent.
    CHECK_FALSE(minimal::client::HandleGlobalKey(ctx, ftxui::Event::Escape));
    CHECK_FALSE(quit_requested);

    // No suggestions open: the idle tab's Escape is simply not cancel_pressed's
    // busy path (turn_in_flight is false) -- unconsumed either way, but for a
    // different reason, proving the new guard only fires when it should.
    suggestions_open_value = false;
    CHECK_FALSE(minimal::client::HandleGlobalKey(ctx, ftxui::Event::Escape));
}

TEST_CASE("HandleGlobalKey: an idle Escape clears a lingering notice, but only "
          "when there is one",
          "[app_state][keybinding_dispatch][notice]") {
    // THE BUG this guards against: a command-result notice ("theme switched
    // to 'x'", a /cost table) had exactly one clear path (submitting a chat
    // turn) and otherwise sat above the editor indefinitely. Idle Escape was
    // a dead key (see keybinding_dispatch.cpp's comment) -- this gives it a
    // job, on the same "closes the innermost transient thing" ladder the
    // modals already follow.
    ClientFixture fixture;
    fixture.Script("hi");
    fixture.Start();

    AppState app(fixture.client);
    const auto tab = app.OpenTab(fixture.Options("first"));
    app.TabNotice(tab, "theme switched to 'dark'");
    REQUIRE(app.GetDetails(tab).latest_notice.has_value());

    const auto keys = minimal::client::Keybindings::Parse(R"({"turn.cancel": "escape"})");
    const auto keymap = minimal::client::ResolvedKeymap::Build(keys);

    bool show_plugin_modal = false;
    std::vector<minimal::client::plugins::PluginViewEntry> plugin_views;
    std::shared_ptr<ftxui::Component> plugin_modal_child;
    bool quit_requested = false;

    minimal::client::KeyContext ctx{
        app, keymap, tab,
        show_plugin_modal,
        plugin_views, plugin_modal_child,
        [] {}, [] {}, [] {},
        [&] { quit_requested = true; },
        [](const std::string&) {},
        [](std::vector<std::string>) {},
        {}, nullptr, nullptr,   // the chat surface's actions and mouse: none here
        [](const ftxui::Event&) { return false; }};   // the editor claims nothing

    // A notice is showing: idle Escape claims the key and clears it.
    CHECK(minimal::client::HandleGlobalKey(ctx, ftxui::Event::Escape));
    CHECK_FALSE(quit_requested);
    CHECK_FALSE(app.GetDetails(tab).latest_notice.has_value());

    // Nothing left to clear: the SAME key now falls through, unconsumed --
    // proving the consumption above was conditional on the notice, not
    // unconditional on idle Escape.
    CHECK_FALSE(minimal::client::HandleGlobalKey(ctx, ftxui::Event::Escape));
}

TEST_CASE("HandleGlobalKey: an open suggestion list gets Tab before a "
          "focused plugin panel claims it",
          "[app_state][keybinding_dispatch]") {
    // Regression: opening a panel auto-focuses it (FocusPanelOnOpen), and a
    // focused panel's OnEvent used to run BEFORE the event ever reached the
    // editor -- with a panel open, Tab-completion for the editor's own
    // suggestion list was unreachable. suggestions_open generalizes the
    // Escape carve-out above to Tab/arrows/Return, checked ahead of the
    // panel dispatch too.
    ClientFixture fixture;
    fixture.Script("hi");
    fixture.Start();

    AppState app(fixture.client);
    const auto tab = app.OpenTab(fixture.Options("first"));

    const auto keys = minimal::client::Keybindings::Parse("{}");
    const auto keymap = minimal::client::ResolvedKeymap::Build(keys);

    bool show_plugin_modal = false;
    std::vector<minimal::client::plugins::PluginViewEntry> plugin_views;
    plugin_views.push_back(ConsumingPanel("test.panel", /*visible=*/true, /*focused=*/true));
    std::shared_ptr<ftxui::Component> plugin_modal_child;
    bool suggestions_open_value = true;

    minimal::client::KeyContext ctx{
        app, keymap, tab,
        show_plugin_modal,
        plugin_views, plugin_modal_child,
        [] {}, [] {}, [] {}, [] {},
        [](const std::string&) {},
        [](std::vector<std::string>) {},
        {}, nullptr, nullptr,   // the chat surface's actions and mouse: none here
        // The editor claims keys while its list is open (PromptEditor::Claims).
        [&](const ftxui::Event&) { return suggestions_open_value; }};

    // The focused panel's component consumes every event -- proven below,
    // once the carve-out is disabled, so the fixture itself is trustworthy.
    CHECK_FALSE(minimal::client::HandleGlobalKey(ctx, ftxui::Event::Tab));

    suggestions_open_value = false;
    CHECK(minimal::client::HandleGlobalKey(ctx, ftxui::Event::Tab));   // the panel claims it
}

TEST_CASE("MakeMentionSuggestor: a bare @ queries the root listing, and tabs "
          "do not clobber each other's answers",
          "[app_state][mention][regression]") {
    // TWO regressions in one e2e case, both from the suggestor's old shape:
    //
    // 1. BARE '@' NEVER LISTED ANYTHING. The lambda short-circuited an empty
    //    prefix to a synchronous empty vector ("nothing after @ yet"), so the
    //    list could never open -- while the SERVER's resolver already answers
    //    "" with the project-root entries (bash-completion shape; see
    //    mention_resolver_test.cpp "an empty fragment lists the project
    //    root"). Dropped: "" goes over the wire like any prefix.
    //
    // 2. ONE SHARED SLOT. The old in-flight tracking was a single struct for
    //    the whole app: tab A's answer was dropped whenever tab B (or A
    //    re-armed on refocus) superseded the slot before the answer was
    //    consumed -- "@ works sometimes". MakeMentionSuggestor keys a slot
    //    PER TAB, so two tabs can each hold an unanswered query.
    //
    // Through the real AppState + ClientFixture server (no seam around
    // RequestSuggestPaths), so the wire format and callback threading are
    // the production ones.
    ClientFixture fixture;
    fixture.Script("hi");
    fixture.Start();

    // Root listing fodder for BOTH tabs' queries.
    { std::ofstream out(fixture.dir / "work" / "README.md"); out << "y"; }
    std::filesystem::create_directories(fixture.dir / "work" / "src");
    { std::ofstream out(fixture.dir / "work" / "main.cpp"); out << "int main() {}"; }

    AppState app(fixture.client);
    const auto tab_a = app.OpenTab(fixture.Options("a"));
    const auto tab_b = app.OpenTab(fixture.Options("b"));
    REQUIRE(app.HasTab(tab_a));
    REQUIRE(app.HasTab(tab_b));

    minimal::client::MentionQuerySlots slots;
    auto suggest = minimal::client::MakeMentionSuggestor(
        &app, slots, [&](const std::string&) {
            return (fixture.dir / "work").string();   // sessionless scope
        });

    // --- bare '@' on tab A: empty prefix must reach the server -----------
    app.SetFocusedTab(tab_a);
    const auto first = suggest("");
    if (first.has_value()) {
        // A present result is only legal when it is the answer; the old code
        // returned an empty vector here, which is exactly what this case
        // forbids for a non-empty root.
        REQUIRE_FALSE(first->empty());
    }
    // Poll until the answer lands (the RPC runs on the async worker).
    std::optional<std::vector<std::string>> answer;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        answer = suggest("");
        if (answer.has_value()) break;
        std::this_thread::sleep_for(10ms);
    }
    REQUIRE(answer.has_value());
    REQUIRE(answer->size() == 3);
    CHECK((*answer)[0] == "README.md");
    CHECK((*answer)[1] == "main.cpp");
    CHECK((*answer)[2] == "src/");

    // --- tab B's query must not supersede A's unanswered one -------------
    // Arm a DIFFERENT prefix for B while A's "src" query is in flight, then
    // let A's answer arrive: with the shared slot it was dropped (the slot
    // now belonged to B) and A polled forever.
    app.SetFocusedTab(tab_b);
    CHECK_FALSE(suggest("R").has_value());   // B arms "R"; nothing yet
    app.SetFocusedTab(tab_a);
    CHECK_FALSE(suggest("src").has_value());   // A arms "src"; nothing yet
    // B polls ITS prefix -- must not consume A's eventual answer.
    app.SetFocusedTab(tab_b);
    std::optional<std::vector<std::string>> b_answer;
    const auto b_deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < b_deadline) {
        b_answer = suggest("R");
        if (b_answer.has_value()) break;
        std::this_thread::sleep_for(10ms);
    }
    REQUIRE(b_answer.has_value());
    REQUIRE(b_answer->size() == 1);
    CHECK((*b_answer)[0] == "README.md");

    // A's own query still resolves under the per-tab slot: back on A, poll
    // the SAME "src" prefix and expect src/'s listing, not B's answer.
    app.SetFocusedTab(tab_a);
    std::optional<std::vector<std::string>> a_answer;
    const auto a_deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < a_deadline) {
        a_answer = suggest("src");
        if (a_answer.has_value()) break;
        std::this_thread::sleep_for(10ms);
    }
    REQUIRE(a_answer.has_value());
    REQUIRE(a_answer->size() == 1);
    CHECK((*a_answer)[0] == "src/");
}

TEST_CASE("TabActivityFeed: a new snapshot only when something in it changed",
          "[app_state][tab_activity]") {
    ClientFixture fixture;
    fixture.Script("hi");
    fixture.Start();
    AppState app(fixture.client);
    client::TabActivityFeed feed;

    const auto empty = feed.Get(app);
    CHECK(empty->tabs.empty());
    CHECK(feed.Get(app) == empty);   // nothing changed: the same snapshot

    const auto tab = app.OpenTab(fixture.Options("first"));
    const auto opened = feed.Get(app);
    REQUIRE(opened->tabs.size() == 1);
    CHECK(opened->version > empty->version);
    CHECK(opened->tabs[0].tab_id == tab);
    CHECK(opened->tabs[0].state == "idle");
    CHECK(feed.Get(app) == opened);
}
