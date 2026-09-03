#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <string>
#include <thread>

#include <unistd.h>

#include <catch2/catch_test_macros.hpp>

#include "fakes.hpp"
#include "minimal/agent/session_registry.hpp"
#include "minimal/common/logger.hpp"
#include "session_events.hpp"
#include "temp_dir.hpp"

using namespace minimal;
using namespace std::chrono_literals;

namespace {

// Session's public surface no longer owns Subscribe/Unsubscribe (Phase 4:
// SessionServiceImpl owns them against the transport directly); these mirror
// what it does, for tests that just need a subscriber present.
rpc::EventBroadcaster::Handle Subscribe(agent::Session& session) {
    auto handle = testing::Transport(session).queue().Subscribe();
    session.NotifyAttached();
    return handle;
}

void Unsubscribe(agent::Session& session, const rpc::EventBroadcaster::Handle& handle) {
    testing::Transport(session).queue().Unsubscribe(handle);
    session.NotifyDetached();
}

// A throwaway config directory, unique per process: catch_discover_tests runs
// every TEST_CASE as its own ctest entry, so a fixed path would be wiped out
// from under a concurrent run of this very file (ctest -j).
struct TempConfigDir {
    testing::TempDir tempdir;
    std::filesystem::path dir = tempdir.path();

    // TempDir owns the arena and removes it; no explicit destructor needed.
    TempConfigDir() { std::filesystem::create_directories(dir / "agents"); }

    void Write(const std::filesystem::path& relative, const std::string& contents) const {
        const auto path = dir / relative;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path) << contents;
    }

    void WriteDefaults(const std::string& provider_kind = "fake") const {
        Write("providers.json",
              nlohmann::json{{"providers",
                              {{"main",
                                {{"backend", provider_kind},
                                 {"url", "http://localhost:8080/v1"},
                                 {"models", {{"test-model", nlohmann::json::object()}}}}}}}}
                  .dump());
        Write("settings.json", "{}");
        Write("permissions.json", "{}");
        Write("agents/build.md",
              "---\nname: build\nmodel: main/test-model\n---\nYou are a build agent.\n");
    }
};

// Everything SessionRegistry borrows, in one place and destroyed after it.
struct RegistryFixture {
    explicit RegistryFixture(std::chrono::milliseconds guard = std::chrono::seconds(10))
        : skills(config::SkillRegistry::Load(config.dir)), guard_(guard) {}

    void Build(bool register_provider_kind = true) {
        presets = std::make_unique<agent::AgentPresetRegistry>(
            config.dir, skills, config::LoadSettingsConfig(config.dir / "settings.json"));
        if (register_provider_kind) {
            providers.Register("fake", [](const llm::ProviderConfig&) {
                return std::make_unique<testing::FakeLlmProvider>();
            });
        }
        observers = std::make_unique<agent::SessionObserverRegistry>(common::DefaultLogger());
        // A real database, but no rows: these tests never run a turn, and
        // SqliteHistoryStore does not touch the connection until it appends.
        // What is being tested is registry lifetime, not persistence.
        db = std::make_unique<session::SessionDatabase>(db_dir.path() / "sessions.db");
        registry = std::make_unique<agent::SessionRegistry>(*presets, tools_registry, providers,
                                                            *observers, *db, agent::LoopConfig{},
                                                            common::DefaultLogger(), guard_);
    }

    ~RegistryFixture() { registry.reset(); }

    TempConfigDir config;
    config::SkillRegistry skills;
    tools::ToolRegistry tools_registry;
    llm::ProviderFactoryRegistry providers;
    std::unique_ptr<agent::AgentPresetRegistry> presets;
    std::unique_ptr<agent::SessionObserverRegistry> observers;
    testing::TempDir db_dir;
    std::unique_ptr<session::SessionDatabase> db;
    std::unique_ptr<agent::SessionRegistry> registry;
    std::chrono::milliseconds guard_;
};

agent::SessionParams Params(std::string id, std::string preset = "build") {
    agent::SessionParams params;
    params.session_id = std::move(id);
    params.agent_preset = std::move(preset);
    params.project_root = std::filesystem::temp_directory_path();
    return params;
}

}  // namespace

TEST_CASE("creating a session resolves its preset and provider", "[session_registry]") {
    RegistryFixture fixture;
    fixture.config.WriteDefaults();
    fixture.Build();

    auto session = fixture.registry->Create(Params("s1"));
    REQUIRE(session != nullptr);
    CHECK(fixture.registry->size() == 1);
    CHECK(fixture.registry->Find("s1") == session);
    CHECK(session->info().session_id == "s1");
}

TEST_CASE("a user-chosen model outranks the preset's", "[session_registry][invariant]") {
    // THE PRECEDENCE RULE, and the correction that shaped this change: /agent and
    // /model are two knobs, not one. A preset supplies a DEFAULT model; a model
    // the user named is a decision, and it wins. Tested here, before /model
    // exists, because --model already exercises the same path -- and because
    // /agent will re-resolve the preset and must not quietly undo it.
    RegistryFixture fixture;
    fixture.config.Write("providers.json",
                         nlohmann::json{{"providers",
                                         {{"main",
                                           {{"backend", "fake"},
                                            {"url", "http://localhost:8080/v1"},
                                            {"max_tokens", 4096},
                                            {"models",
                                             {{"test-model", nlohmann::json::object()},
                                              {"other-model", {{"max_tokens", 512}}}}}}}}}}
                             .dump());
    fixture.config.Write("settings.json", "{}");
    fixture.config.Write("permissions.json", "{}");
    fixture.config.Write("agents/build.md",
                         "---\nname: build\nmodel: main/test-model\n---\nbody\n");
    fixture.Build();

    auto params = Params("s1");
    params.model_override = minimal::common::ModelRef{"main", "other-model"};
    auto session = fixture.registry->Create(std::move(params));
    REQUIRE(session != nullptr);

    // The preset still says test-model; this session runs other-model, with that
    // model's own cap rather than the provider default the preset resolved to.
    CHECK(fixture.presets->Resolve("build").provider.model_name == "test-model");
    CHECK(session->preset().provider.model_name == "other-model");
    REQUIRE(session->preset().max_tokens.has_value());
    CHECK(*session->preset().max_tokens == 512);
}

TEST_CASE("an unknown model override is named, with what is available",
          "[session_registry]") {
    RegistryFixture fixture;
    fixture.config.WriteDefaults();
    fixture.Build();

    auto params = Params("s1");
    params.model_override = minimal::common::ModelRef{"main", "nope"};
    try {
        fixture.registry->Create(std::move(params));
        FAIL("expected an unknown model to throw");
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        CHECK(message.find("main/nope") != std::string::npos);
        CHECK(message.find("main/test-model") != std::string::npos);
    }
    CHECK(fixture.registry->size() == 0);
}

TEST_CASE("an unknown preset fails loudly", "[session_registry]") {
    RegistryFixture fixture;
    fixture.config.WriteDefaults();
    fixture.Build();

    CHECK_THROWS_AS(fixture.registry->Create(Params("s1", "nonexistent")), std::runtime_error);
    CHECK(fixture.registry->size() == 0);
}

TEST_CASE("a provider kind no plugin registered is named, not a null deref",
          "[session_registry]") {
    // AgentPreset carries provider_kind precisely so this check is possible.
    // Without it a session would construct with a null provider and fail on its
    // first turn for no stated reason.
    RegistryFixture fixture;
    fixture.config.WriteDefaults("kind-nobody-registered");
    fixture.Build(/*register_provider_kind=*/false);

    try {
        fixture.registry->Create(Params("s1"));
        FAIL("expected a throw naming the missing provider kind");
    } catch (const std::runtime_error& e) {
        const std::string message = e.what();
        INFO(message);
        CHECK(message.find("kind-nobody-registered") != std::string::npos);
    }
    CHECK(fixture.registry->size() == 0);
}

TEST_CASE("terminating removes the session from the registry", "[session_registry]") {
    RegistryFixture fixture;
    fixture.config.WriteDefaults();
    fixture.Build();

    auto session = fixture.registry->Create(Params("s1"));
    fixture.registry->Terminate("s1");
    CHECK(fixture.registry->size() == 0);
    CHECK(fixture.registry->Find("s1") == nullptr);
    CHECK(session->terminated());
}

TEST_CASE("terminating an unknown id is a no-op", "[session_registry]") {
    RegistryFixture fixture;
    fixture.config.WriteDefaults();
    fixture.Build();
    fixture.registry->Terminate("never-existed");
    CHECK(fixture.registry->size() == 0);
}

TEST_CASE("a never-attached session is reaped by the initial-attach guard",
          "[session_registry]") {
    // A SAFETY NET, not a designed flow: attaching is a client protocol
    // obligation, and this only fires against a client that failed it.
    RegistryFixture fixture(200ms);
    fixture.config.WriteDefaults();
    fixture.Build();

    auto session = fixture.registry->Create(Params("s1"));
    CHECK(fixture.registry->size() == 1);

    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (fixture.registry->size() > 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    CHECK(fixture.registry->size() == 0);
    CHECK(session->terminated());
}

TEST_CASE("an attached session outlives the guard", "[session_registry]") {
    // The rule is not "unattached => terminate". A subagent emits events before
    // its tab attaches, and an attached session must never be reaped for being
    // idle -- only for never having been attached at all.
    RegistryFixture fixture(200ms);
    fixture.config.WriteDefaults();
    fixture.Build();

    auto session = fixture.registry->Create(Params("s1"));
    const auto subscriber = Subscribe(*session);

    std::this_thread::sleep_for(700ms);  // several guard periods
    CHECK(fixture.registry->size() == 1);
    CHECK_FALSE(session->terminated());
}

TEST_CASE("two detached quiescent sessions are both reaped by the idle TTL, on schedule",
          "[session_registry]") {
    // The bug: erasing session A from sessions_ advances `it` to session B (the
    // only other live entry), and the old code then erased B's quiescent_since_
    // entry instead of A's. B's clock resets to "now", so B is NOT reaped on
    // schedule -- it needs a second full detached_idle_ttl before it is finally
    // reaped. The 1.6s deadline below cleanly separates "both reaped within
    // ~1.1s" (fixed) from "one needs a second full TTL, ~2s" (buggy).
    RegistryFixture fixture(200ms);
    fixture.config.WriteDefaults();
    fixture.Build();

    agent::SessionRegistry::ReaperConfig config;
    config.initial_attach_guard = 200ms;
    config.detached_idle_ttl = std::chrono::seconds(1);
    config.idle_shutdown = std::chrono::seconds(0);
    fixture.registry->set_reaper_config(config);

    auto first = fixture.registry->Create(Params("s1"));
    auto second = fixture.registry->Create(Params("s2"));

    // Attach then detach both -- ever_attached() latches true, has_subscribers()
    // drops back to false: exactly the detached-quiescent state the reaper
    // reaps.
    {
        const auto first_sub = Subscribe(*first);
        Unsubscribe(*first, first_sub);
        const auto second_sub = Subscribe(*second);
        Unsubscribe(*second, second_sub);
    }
    CHECK(fixture.registry->size() == 2);

    // size() drops to 0 as soon as a doomed session is erased from the
    // registry's map, but Terminate() (which sets terminated()) runs
    // afterward, outside the registry lock -- so the deadline loop must wait
    // on terminated() too, not just size(), or this races against Terminate()
    // completing (visible under asan/tsan's slower execution).
    const auto deadline = std::chrono::steady_clock::now() + 1600ms;
    while ((fixture.registry->size() > 0 || !first->terminated() || !second->terminated()) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(20ms);
    }
    CHECK(fixture.registry->size() == 0);
    CHECK(first->terminated());
    CHECK(second->terminated());
}

// --- Idle exit: the Ping half -------------------------------------------------

TEST_CASE("the idle exit holds while a session-less client keeps pinging", "[session_registry]") {
    // Regression: the idle exit fired once zero sessions were live and zero
    // streams attached. A live client that holds NEITHER -- the TUI's
    // empty-state welcome view, before its first session -- attached no
    // stream and created no session, so the server shut itself down under
    // the user (the reported "start client, fiddle with /-autocomplete,
    // submit, server stops"). With client_idle_for wired, a fresh Ping
    // every beat holds the exit off indefinitely.
    RegistryFixture fixture(200ms);   // 50ms reaper tick
    fixture.config.WriteDefaults();
    fixture.Build();

    std::atomic<int> idle_exits{0};
    agent::SessionRegistry::ReaperConfig config;
    config.initial_attach_guard = 200ms;   // 50ms reaper tick
    config.idle_shutdown = std::chrono::seconds(1);
    config.attached_clients = [] { return 0; };
    config.client_idle_for = [] {
        return std::chrono::milliseconds(0);   // pinged just now, every beat
    };
    config.on_idle_expired = [&idle_exits] { idle_exits.fetch_add(1); };
    fixture.registry->set_reaper_config(config);

    std::this_thread::sleep_for(1600ms);   // several idle_shutdown periods
    CHECK(idle_exits.load() == 0);
}

TEST_CASE("the idle exit fires when the last client ping has gone stale", "[session_registry]") {
    // The counterpart: an absent or long-gone client (its heartbeat stopped
    // -- the TUI quit) must still let the server exit on its linger. The
    // old two-halves behavior is unchanged; this only proves the Ping half
    // does not wedge the exit open forever.
    RegistryFixture fixture(200ms);
    fixture.config.WriteDefaults();
    fixture.Build();

    std::atomic<int> idle_exits{0};
    agent::SessionRegistry::ReaperConfig config;
    config.initial_attach_guard = 200ms;   // 50ms reaper tick
    config.idle_shutdown = std::chrono::seconds(1);
    config.attached_clients = [] { return 0; };
    config.client_idle_for = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::seconds(3600));   // last beat: an hour ago
    };
    config.on_idle_expired = [&idle_exits] { idle_exits.fetch_add(1); };
    fixture.registry->set_reaper_config(config);

    const auto deadline = std::chrono::steady_clock::now() + 2500ms;
    while (idle_exits.load() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(20ms);
    }
    CHECK(idle_exits.load() > 0);
}

TEST_CASE("the reaper calls the configured periodic checkpoint callback, not the database directly",
          "[session_registry]") {
    // ReaperConfig::periodic_checkpoint replaces a direct SessionDatabase&
    // call on every reaper tick -- a fake-store-backed registry (no real WAL)
    // must be able to observe the tick fire without a live SQLite file.
    RegistryFixture fixture(200ms);
    fixture.config.WriteDefaults();
    fixture.Build();

    std::atomic<int> calls{0};
    agent::SessionRegistry::ReaperConfig config;
    config.initial_attach_guard = 200ms;
    config.periodic_checkpoint = [&calls] { calls.fetch_add(1); };
    fixture.registry->set_reaper_config(config);

    const auto deadline = std::chrono::steady_clock::now() + 1000ms;
    while (calls.load() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(20ms);
    }
    CHECK(calls.load() > 0);
}

TEST_CASE("TerminateAll stops every live session", "[session_registry]") {
    RegistryFixture fixture;
    fixture.config.WriteDefaults();
    fixture.Build();

    auto first = fixture.registry->Create(Params("s1"));
    auto second = fixture.registry->Create(Params("s2"));
    const auto first_sub = Subscribe(*first);
    const auto second_sub = Subscribe(*second);
    CHECK(fixture.registry->size() == 2);

    fixture.registry->TerminateAll();
    CHECK(fixture.registry->size() == 0);
    CHECK(first->terminated());
    CHECK(second->terminated());
}

TEST_CASE("sessions are independent", "[session_registry]") {
    RegistryFixture fixture;
    fixture.config.WriteDefaults();
    fixture.Build();

    auto first = fixture.registry->Create(Params("s1"));
    auto second = fixture.registry->Create(Params("s2"));
    const auto first_sub = Subscribe(*first);
    const auto second_sub = Subscribe(*second);

    fixture.registry->Terminate("s1");
    CHECK(first->terminated());
    CHECK_FALSE(second->terminated());
    CHECK(fixture.registry->Find("s2") == second);
}

// --- Resume ------------------------------------------------------------------

TEST_CASE("resume loads the stored transcript and repairs a torn tool tail",
          "[session_registry][resume]") {
    // A session cancelled mid-tool-execution ends with an unanswered tool_use.
    // Resume must persist synthesized error results for it BEFORE the session
    // starts, or the first provider call of the resumed session is a hard 400.
    RegistryFixture fixture;
    fixture.config.WriteDefaults();
    fixture.Build();

    const auto row = fixture.db->CreateSession("torn", std::nullopt, "/proj", "build");
    fixture.db->AppendMessage(row.session_id,
                              llm::Message{llm::Role::User, {llm::TextBlock{"run it"}}});
    llm::Message tool_use{llm::Role::Assistant, {}};
    tool_use.content.push_back(llm::ToolUseBlock{"call-1", "bash", {{"command", "make"}}});
    fixture.db->AppendMessage(row.session_id, tool_use);
    // ...and nothing after it: the session died before results were appended.

    auto params = Params(row.session_id);
    params.resume = true;
    auto session = fixture.registry->Create(std::move(params));
    REQUIRE(session != nullptr);

    const auto loaded = fixture.db->LoadMessages(row.session_id);
    REQUIRE(loaded.size() == 3);
    REQUIRE(loaded[2].message.role == llm::Role::User);
    REQUIRE(loaded[2].message.content.size() == 1);
    const auto* repair = std::get_if<llm::ToolResultBlock>(&loaded[2].message.content.front());
    REQUIRE(repair != nullptr);
    CHECK(repair->tool_use_id == "call-1");
    CHECK(repair->is_error);
}

TEST_CASE("resume of a clean transcript appends nothing", "[session_registry][resume]") {
    RegistryFixture fixture;
    fixture.config.WriteDefaults();
    fixture.Build();

    const auto row = fixture.db->CreateSession("clean", std::nullopt, "/proj", "build");
    fixture.db->AppendMessage(row.session_id,
                              llm::Message{llm::Role::User, {llm::TextBlock{"hi"}}});
    fixture.db->AppendMessage(
        row.session_id,
        llm::Message{llm::Role::Assistant, {llm::TextBlock{"hello"}}}, 5);

    auto params = Params(row.session_id);
    params.resume = true;
    auto session = fixture.registry->Create(std::move(params));
    REQUIRE(session != nullptr);

    // Exactly the stored rows -- resume itself never re-appends history.
    CHECK(fixture.db->LoadMessages(row.session_id).size() == 2);
}

TEST_CASE("resume with a compaction checkpoint reconstructs the compacted view",
          "[session_registry][resume][checkpoint]") {
    // The checkpoint is the cached view the live session left behind. Resume
    // must seed the loop with [summary + tail] -- what the live session's
    // working set actually was -- not the full uncompacted transcript, and it
    // must not spend a provider call re-summarizing on the way in.
    RegistryFixture fixture;
    fixture.config.WriteDefaults();

    // Capture the provider instance the resumed session constructs, so the
    // first request can be inspected after the turn.
    testing::FakeLlmProvider* provider = nullptr;
    fixture.providers.Register("fake", [&provider](const llm::ProviderConfig&) {
        auto owned = std::make_unique<testing::FakeLlmProvider>();
        provider = owned.get();
        return owned;
    });
    fixture.Build(/*register_provider_kind=*/false);

    const auto row = fixture.db->CreateSession("ckpt", std::nullopt, "/proj", "build");
    fixture.db->AppendMessage(row.session_id,
                              llm::Message{llm::Role::User, {llm::TextBlock{"old-1"}}});
    fixture.db->AppendMessage(row.session_id,
                              llm::Message{llm::Role::Assistant, {llm::TextBlock{"old-2"}}});
    fixture.db->AppendMessage(row.session_id,
                              llm::Message{llm::Role::User, {llm::TextBlock{"tail"}}});
    // The live session compacted everything through seq 1 into one summary.
    fixture.db->SaveCompactionCheckpoint(
        row.session_id, 1, llm::Message{llm::Role::User, {llm::TextBlock{"[compacted]"}}});

    auto params = Params(row.session_id);
    params.resume = true;
    auto session = fixture.registry->Create(std::move(params));
    REQUIRE(session != nullptr);

    REQUIRE(provider != nullptr);
    provider->PushResponse(testing::TextResponse("ok"));

    session->SubmitTurn("new");
    REQUIRE(session->WaitForIdleForTesting(5s));

    // Exactly one provider call -- the turn itself, no summarization -- and
    // its context is [summary, tail, turn], not the four stored messages.
    CHECK(provider->call_count() == 1);
    const auto& requests = provider->requests();
    REQUIRE(requests.size() == 1);
    REQUIRE(requests[0].messages.size() == 3);
    CHECK(std::get<llm::TextBlock>(requests[0].messages[0].content.front()).text == "[compacted]");
    CHECK(std::get<llm::TextBlock>(requests[0].messages[1].content.front()).text == "tail");
    CHECK(std::get<llm::TextBlock>(requests[0].messages[2].content.front()).text == "new");
}

TEST_CASE("resume of an already-open session is refused", "[session_registry][resume]") {
    // Backstop: two live Sessions on one transcript would interleave appends.
    RegistryFixture fixture;
    fixture.config.WriteDefaults();
    fixture.Build();

    const auto row = fixture.db->CreateSession("live", std::nullopt, "/proj", "build");
    auto params = Params(row.session_id);
    params.resume = true;
    REQUIRE(fixture.registry->Create(std::move(params)) != nullptr);
    auto dup = Params(row.session_id);
    dup.resume = true;
    CHECK_THROWS_AS(fixture.registry->Create(std::move(dup)), agent::SessionAlreadyOpen);
    CHECK(fixture.registry->size() == 1);
}

TEST_CASE("a concurrent duplicate construction is refused before any side effect",
          "[session_registry][resume][invariant]") {
    // THE RESERVATION: the id is claimed under the registry mutex before any
    // construction work runs. The old guard checked at the END of
    // construction, so two concurrent resumes both loaded the transcript and
    // both appended a torn-tail repair row before the loser was refused.
    // Here the winner parks INSIDE CreateResolved -- after the reservation,
    // before the transcript load -- making the loser's refusal observable
    // while the winner is provably mid-construction.
    RegistryFixture fixture;
    fixture.config.WriteDefaults();

    std::promise<void> factory_entered;
    std::promise<void> factory_release;
    fixture.providers.Register("fake", [&, entered = false](const llm::ProviderConfig&) mutable {
        if (!entered) {
            entered = true;
            factory_entered.set_value();
            factory_release.get_future().wait();
        }
        return std::make_unique<testing::FakeLlmProvider>();
    });
    fixture.Build(/*register_provider_kind=*/false);

    // A TORN tail: if the loser got past the reservation it would append a
    // synthesized repair row, and the count below would expose it.
    const auto row = fixture.db->CreateSession("raced", std::nullopt, "/proj", "build");
    fixture.db->AppendMessage(row.session_id,
                              llm::Message{llm::Role::User, {llm::TextBlock{"run it"}}});
    llm::Message tool_use{llm::Role::Assistant, {}};
    tool_use.content.push_back(llm::ToolUseBlock{"call-1", "bash", {{"command", "make"}}});
    fixture.db->AppendMessage(row.session_id, tool_use);

    auto winner_params = Params(row.session_id);
    winner_params.resume = true;
    std::thread winner([&] { (void)fixture.registry->Create(std::move(winner_params)); });
    factory_entered.get_future().wait();

    auto loser_params = Params(row.session_id);
    loser_params.resume = true;
    CHECK_THROWS_AS(fixture.registry->Create(std::move(loser_params)),
                    agent::SessionAlreadyOpen);
    // The loser never touched the transcript: no repair row, no duplicate.
    CHECK(fixture.db->LoadMessages(row.session_id).size() == 2);

    factory_release.set_value();
    winner.join();
    CHECK(fixture.registry->Find(row.session_id) != nullptr);
    // The winner's own resume repair landed exactly once.
    CHECK(fixture.db->LoadMessages(row.session_id).size() == 3);
}

// --- Subagents -------------------------------------------------------------

TEST_CASE("CreateResolved uses the preset it is handed", "[session_registry][subagent]") {
    // The subagent spawner adapts a resolved preset before the session exists
    // (is_subagent, prompts/subagent.md prefix, intersected allowed lists); no name-based
    // lookup can express that. The registry must take the preset as given --
    // here proven by handing it a system prompt no file on disk contains.
    RegistryFixture fixture;
    fixture.config.WriteDefaults();
    fixture.Build();

    agent::AgentPreset adapted = fixture.presets->Resolve("build");
    adapted.is_subagent = true;
    adapted.system_prompt = "[subagent] " + adapted.system_prompt;

    auto session = fixture.registry->CreateResolved(Params("s1"), adapted);
    REQUIRE(session != nullptr);
    CHECK(session->preset().is_subagent);
    CHECK(session->preset().system_prompt.rfind("[subagent] ", 0) == 0);
}

TEST_CASE("ChildCount counts only live sessions of that parent", "[session_registry][subagent]") {
    RegistryFixture fixture;
    fixture.config.WriteDefaults();
    fixture.Build();

    auto parent = fixture.registry->Create(Params("p"));
    agent::SessionParams child_a = Params("ca");
    child_a.parent_session_id = "p";
    agent::SessionParams child_b = Params("cb");
    child_b.parent_session_id = "p";
    agent::SessionParams other = Params("other");
    fixture.registry->Create(std::move(child_a));
    fixture.registry->Create(std::move(child_b));
    fixture.registry->Create(std::move(other));

    CHECK(fixture.registry->ChildCount("p") == 2);
    CHECK(fixture.registry->ChildCount("other") == 0);

    // Terminated children stop counting: the concurrency limit is about live
    // subagents, not historical ones.
    fixture.registry->Terminate("ca");
    CHECK(fixture.registry->ChildCount("p") == 1);
}

// --- Project context (AGENTS.md / CLAUDE.md) ---------------------------------

TEST_CASE("project context is loaded from the session's project_root into the system prompt",
          "[session_registry][project_context]") {
    RegistryFixture fixture;
    fixture.config.WriteDefaults();
    fixture.Build();
    // Explicit wiring turns the feature on; the registry defaults to disabled
    // so unwired tests never scan the filesystem.
    fixture.registry->set_project_context(config::ProjectContextSettings{});

    testing::TempDir project;
    { std::ofstream(project.path() / "AGENTS.md") << "Always run the tests.\n"; }

    auto params = Params("s1");
    params.project_root = project.path();
    auto session = fixture.registry->Create(std::move(params));
    REQUIRE(session != nullptr);

    const auto request = session->loop().BuildChatRequest();
    REQUIRE(request.system.has_value());
    CHECK(request.system->find("Instructions from AGENTS.md in the project root:") !=
          std::string::npos);
    CHECK(request.system->find("Always run the tests.") != std::string::npos);
    // Appended after the preset body, which stays at the front.
    CHECK(request.system->rfind("You are a build agent.", 0) == 0);
}

TEST_CASE("project context falls back to CLAUDE.md and skips empty files",
          "[session_registry][project_context]") {
    RegistryFixture fixture;
    fixture.config.WriteDefaults();
    fixture.Build();
    fixture.registry->set_project_context(config::ProjectContextSettings{});

    testing::TempDir project;
    { std::ofstream(project.path() / "AGENTS.md") << ""; }
    { std::ofstream(project.path() / "CLAUDE.md") << "Claude-side instructions.\n"; }

    auto params = Params("s1");
    params.project_root = project.path();
    auto session = fixture.registry->Create(std::move(params));

    const auto request = session->loop().BuildChatRequest();
    REQUIRE(request.system.has_value());
    CHECK(request.system->find("Instructions from CLAUDE.md in the project root:") !=
          std::string::npos);
    CHECK(request.system->find("Claude-side instructions.") != std::string::npos);
}

TEST_CASE("project context is absent when disabled or when no candidate file exists",
          "[session_registry][project_context]") {
    RegistryFixture fixture;
    fixture.config.WriteDefaults();
    fixture.Build();

    SECTION("registry defaults: the feature is off until wired") {
        auto session = fixture.registry->Create(Params("s1"));
        const auto request = session->loop().BuildChatRequest();
        REQUIRE(request.system.has_value());
        CHECK(request.system->find("## Project context") == std::string::npos);
    }
    SECTION("enabled, but the project has neither file") {
        fixture.registry->set_project_context(config::ProjectContextSettings{});
        auto session = fixture.registry->Create(Params("s1"));
        const auto request = session->loop().BuildChatRequest();
        REQUIRE(request.system.has_value());
        CHECK(request.system->find("## Project context") == std::string::npos);
    }
}
