#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

#include <unistd.h>

#include <catch2/catch_test_macros.hpp>

#include "fakes.hpp"
#include "minimal/agent/session_registry.hpp"
#include "minimal/common/logger.hpp"
#include "temp_dir.hpp"

using namespace minimal;
using namespace std::chrono_literals;

namespace {

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
    auto session = fixture.registry->Create(params);
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
        fixture.registry->Create(params);
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
    session->TryAttach();

    std::this_thread::sleep_for(700ms);  // several guard periods
    CHECK(fixture.registry->size() == 1);
    CHECK_FALSE(session->terminated());
}

TEST_CASE("TerminateAll stops every live session", "[session_registry]") {
    RegistryFixture fixture;
    fixture.config.WriteDefaults();
    fixture.Build();

    auto first = fixture.registry->Create(Params("s1"));
    auto second = fixture.registry->Create(Params("s2"));
    first->TryAttach();
    second->TryAttach();
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
    first->TryAttach();
    second->TryAttach();

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
    auto session = fixture.registry->Create(params);
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
    auto session = fixture.registry->Create(params);
    REQUIRE(session != nullptr);

    // Exactly the stored rows -- resume itself never re-appends history.
    CHECK(fixture.db->LoadMessages(row.session_id).size() == 2);
}

TEST_CASE("resume of an already-open session is refused", "[session_registry][resume]") {
    // Backstop: two live Sessions on one transcript would interleave appends.
    RegistryFixture fixture;
    fixture.config.WriteDefaults();
    fixture.Build();

    const auto row = fixture.db->CreateSession("live", std::nullopt, "/proj", "build");
    auto params = Params(row.session_id);
    params.resume = true;
    REQUIRE(fixture.registry->Create(params) != nullptr);
    CHECK_THROWS_AS(fixture.registry->Create(params), std::runtime_error);
    CHECK(fixture.registry->size() == 1);
}

// --- Subagents -------------------------------------------------------------

TEST_CASE("CreateResolved uses the preset it is handed", "[session_registry][subagent]") {
    // The subagent spawner adapts a resolved preset before the session exists
    // (is_subagent, subagent.md prefix, intersected allowed lists); no name-based
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
    fixture.registry->Create(child_a);
    fixture.registry->Create(child_b);
    fixture.registry->Create(other);

    CHECK(fixture.registry->ChildCount("p") == 2);
    CHECK(fixture.registry->ChildCount("other") == 0);

    // Terminated children stop counting: the concurrency limit is about live
    // subagents, not historical ones.
    fixture.registry->Terminate("ca");
    CHECK(fixture.registry->ChildCount("p") == 1);
}
