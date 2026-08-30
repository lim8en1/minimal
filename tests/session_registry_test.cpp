#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "fakes.hpp"
#include "minimal/agent/session_registry.hpp"
#include "minimal/common/logger.hpp"

using namespace minimal;
using namespace std::chrono_literals;

namespace {

// A separate arena from agent_preset_registry_test's: two fixtures sharing one
// fixed path would have whichever ran second wipe the first.
struct TempConfigDir {
    std::filesystem::path dir;

    TempConfigDir() {
        dir = std::filesystem::temp_directory_path() / "minimal_session_registry_test_arena";
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir / "agents");
    }
    ~TempConfigDir() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    void Write(const std::filesystem::path& relative, const std::string& contents) const {
        const auto path = dir / relative;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path) << contents;
    }

    void WriteDefaults(const std::string& provider_kind = "fake") const {
        Write("providers.json", nlohmann::json::array({{{"name", "main"},
                                                        {"provider", provider_kind},
                                                        {"base_url", "http://localhost:8080/v1"},
                                                        {"model_name", "test-model"}}})
                                    .dump());
        Write("settings.json", "{}");
        Write("permissions.json", "{}");
        Write("agents/build.md",
              "---\nname: build\nprovider: main\n---\nYou are a build agent.\n");
    }
};

// Everything SessionRegistry borrows, in one place and destroyed after it.
struct RegistryFixture {
    explicit RegistryFixture(std::chrono::milliseconds guard = std::chrono::seconds(10))
        : skills(config::SkillRegistry::Load(config.dir)), guard_(guard) {}

    void Build(bool register_provider_kind = true) {
        presets = std::make_unique<agent::AgentPresetRegistry>(config.dir, skills);
        if (register_provider_kind) {
            providers.Register("fake", [](const llm::ProviderConfig&) {
                return std::make_unique<testing::FakeLlmProvider>();
            });
        }
        observers = std::make_unique<agent::SessionObserverRegistry>(common::DefaultLogger());
        registry = std::make_unique<agent::SessionRegistry>(*presets, tools_registry, providers,
                                                            *observers, agent::LoopConfig{},
                                                            common::DefaultLogger(), guard_);
    }

    ~RegistryFixture() { registry.reset(); }

    TempConfigDir config;
    config::SkillRegistry skills;
    tools::ToolRegistry tools_registry;
    llm::ProviderFactoryRegistry providers;
    std::unique_ptr<agent::AgentPresetRegistry> presets;
    std::unique_ptr<agent::SessionObserverRegistry> observers;
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
    session->MarkAttached();

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
    first->MarkAttached();
    second->MarkAttached();
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
    first->MarkAttached();
    second->MarkAttached();

    fixture.registry->Terminate("s1");
    CHECK(first->terminated());
    CHECK_FALSE(second->terminated());
    CHECK(fixture.registry->Find("s2") == second);
}
