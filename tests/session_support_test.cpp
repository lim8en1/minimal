// SessionSupportService -- the core-provided pull seam for session-lifecycle
// plugins. The interface contract is tested against the real implementation
// over a real SessionRegistry and SessionDatabase: what a titler plugin will
// actually do (resolve the session's own provider, read the transcript, rename
// with notification) is what these tests drive.

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "fakes.hpp"
#include "session_events.hpp"
#include "minimal/agent/session_registry.hpp"
#include "minimal/agent/session_support.hpp"
#include "minimal/common/logger.hpp"
#include "temp_dir.hpp"

using namespace minimal;
using namespace std::chrono_literals;

namespace {

// Same shape as session_registry_test.cpp's fixture: one config dir per test
// process (catch_discover_tests runs every TEST_CASE as its own ctest entry).
struct SupportFixture {
    // First member: the layout must exist before `skills` loads -- member
    // initializers run in declaration order.
    struct Layout {
        explicit Layout(const std::filesystem::path& base) {
            std::filesystem::create_directories(base / "agents");
        }
    };
    testing::TempDir dir;
    Layout layout{dir.path()};
    config::SkillRegistry skills{config::SkillRegistry::Load(dir.path())};
    tools::ToolRegistry tools;
    llm::ProviderFactoryRegistry providers;
    std::unique_ptr<agent::AgentPresetRegistry> presets;
    std::unique_ptr<agent::SessionObserverRegistry> observers;
    std::unique_ptr<session::SessionDatabase> db;
    std::unique_ptr<agent::SessionRegistry> registry;
    std::unique_ptr<agent::SessionSupport> support;

    SupportFixture() {
        Write("providers.json",
              nlohmann::json{{"providers",
                              {{"main",
                                {{"backend", "fake"},
                                 {"url", "http://localhost:8080/v1"},
                                 {"models", {{"test-model", nlohmann::json::object()}}}}}}}}
                  .dump());
        Write("settings.json", "{}");
        Write("permissions.json", "{}");
        Write("agents/build.md",
              "---\nname: build\nmodel: main/test-model\n---\nYou are a build agent.\n");
        presets = std::make_unique<agent::AgentPresetRegistry>(
            dir.path(), skills, config::LoadSettingsConfig(dir.path() / "settings.json"));
        providers.Register("fake", [](const llm::ProviderConfig&) {
            return std::make_unique<testing::FakeLlmProvider>();
        });
        observers = std::make_unique<agent::SessionObserverRegistry>(common::DefaultLogger());
        db = std::make_unique<session::SessionDatabase>(dir.path() / "sessions.db");
        registry = std::make_unique<agent::SessionRegistry>(*presets, tools, providers,
                                                            *observers, *db, agent::LoopConfig{},
                                                            common::DefaultLogger());
        support = std::make_unique<agent::SessionSupport>(common::DefaultLogger());
        support->Wire(*registry, *db, *presets);
    }

    void Write(const std::filesystem::path& relative, const std::string& contents) const {
        const auto path = dir.path() / relative;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path) << contents;
    }

    // A session backed by a REAL row (SessionSupport's rename path reads and
    // writes the database, unlike registry-only tests).
    std::shared_ptr<agent::Session> CreateSession(const std::string& name) {
        const auto row = db->CreateSession(name, std::nullopt, "/p", "build");
        agent::SessionParams params;
        params.session_id = row.session_id;
        params.name = name;
        params.agent_preset = "build";
        params.project_root = std::filesystem::temp_directory_path();
        return registry->Create(std::move(params));
    }
};

// Drains the session's outbound queue, looking for a session_metadata event.
bool QueueHasRename(agent::Session& session, const std::string& name) {
    bool found = false;
    for (const auto& event : testing::DrainEvents(session)) {
        if (event.has_session_metadata() && event.session_metadata().name() == name &&
            event.session_metadata().session_id() == session.session_id()) {
            found = true;
        }
    }
    return found;
}

}  // namespace

TEST_CASE("LlmForSession returns the session's own provider and model",
          "[session_support]") {
    SupportFixture fixture;
    auto session = fixture.CreateSession("");
    REQUIRE(session != nullptr);

    const auto llm = fixture.support->LlmForSession(session->session_id());
    REQUIRE(llm.has_value());
    CHECK(llm->model == "test-model");
    CHECK(llm->provider != nullptr);
    // The same shared instance the session holds -- the cache, not a second
    // connection to the same backend.
    CHECK(llm->provider.get() == session->ProviderSnapshot().get());
}

TEST_CASE("LlmForSession degrades on an unknown session", "[session_support]") {
    SupportFixture fixture;
    CHECK_FALSE(fixture.support->LlmForSession("no-such-session").has_value());
    CHECK(fixture.support->HistoryFor("no-such-session") == nullptr);
    CHECK_FALSE(fixture.support->RenameSession("no-such-session", "x"));
}

TEST_CASE("a DB-backed session gone from the registry still titles and reads",
          "[session_support]") {
    // The race the titler actually hits: a one-shot headless turn closes its
    // stream the instant TurnCompletedEvent lands, and the session can be
    // erased from the registry BEFORE the queued OnTurnCompleted dispatches.
    // The service must fall back to the database -- the authoritative record
    // -- rather than report "no session" for a session that ran a real turn.
    SupportFixture fixture;
    // A real row, no registry session: the exact state a raced teardown leaves.
    const auto row = fixture.db->CreateSession("", std::nullopt, "/p", "build");

    const auto reader = fixture.support->HistoryFor(row.session_id);
    REQUIRE(reader != nullptr);
    // The same class over the same database the live session's reader served.
    const auto page = reader->GetMessages(row.session_id, std::nullopt, 10);
    CHECK(page.messages.empty());

    const auto llm = fixture.support->LlmForSession(row.session_id);
    REQUIRE(llm.has_value());
    CHECK(llm->model == "test-model");
    CHECK(llm->provider != nullptr);

    CHECK(fixture.support->RenameSession(row.session_id, "late title"));
    CHECK(fixture.db->GetSession(row.session_id)->name == "late title");
}

TEST_CASE("ResolveModelRef shares the cached instance and rejects junk",
          "[session_support]") {
    SupportFixture fixture;
    auto session = fixture.CreateSession("");

    const auto resolved = fixture.support->ResolveModelRef("main/test-model");
    REQUIRE(resolved.has_value());
    CHECK(resolved->model == "test-model");
    CHECK(resolved->provider.get() == session->ProviderSnapshot().get());

    // Not a <provider>/<model> ref, and a well-formed ref resolving to nothing.
    CHECK_FALSE(fixture.support->ResolveModelRef("test-model").has_value());
    CHECK_FALSE(fixture.support->ResolveModelRef("main/nope").has_value());
    CHECK_FALSE(fixture.support->ResolveModelRef("no-such-provider/test-model").has_value());
}

TEST_CASE("HistoryFor hands out the session's own transcript reader",
          "[session_support]") {
    SupportFixture fixture;
    auto session = fixture.CreateSession("");
    const auto reader = fixture.support->HistoryFor(session->session_id());
    REQUIRE(reader != nullptr);
    // The same reader the session's tools see through ToolContext::history.
    CHECK(reader.get() == session->history_reader().get());
}

TEST_CASE("RenameSession persists, updates the live session, and notifies",
          "[session_support][naming]") {
    SupportFixture fixture;
    auto session = fixture.CreateSession("");
    REQUIRE(session != nullptr);

    CHECK(fixture.support->RenameSession(session->session_id(), "a generated title"));

    // Database: the authoritative record.
    CHECK(fixture.db->GetSession(session->session_id())->name == "a generated title");
    // Live session: later observer payloads and info() readers see it.
    CHECK(session->info().name == "a generated title");
    // Wire: the attached client is told, so a tab label refreshes.
    CHECK(QueueHasRename(*session, "a generated title"));
}

TEST_CASE("RenameSession refuses a session the user named at creation",
          "[session_support][naming]") {
    // The protection mirroring the free default's `name = ''` guard: a name the
    // user chose is a decision, and a generated title never clobbers it.
    SupportFixture fixture;
    auto session = fixture.CreateSession("chosen by the user");
    REQUIRE(session != nullptr);

    CHECK_FALSE(fixture.support->RenameSession(session->session_id(), "a generated title"));
    CHECK(session->info().name == "chosen by the user");
    CHECK(fixture.db->GetSession(session->session_id())->name == "chosen by the user");
    // No event: nothing changed, so nothing to refresh.
    CHECK(testing::DrainEvents(*session).empty());
}

TEST_CASE("RenameSession refuses an empty name", "[session_support][naming]") {
    SupportFixture fixture;
    auto session = fixture.CreateSession("");
    REQUIRE(session != nullptr);
    CHECK_FALSE(fixture.support->RenameSession(session->session_id(), ""));
    CHECK(session->info().name.empty());
}

TEST_CASE("RenameSession survives a terminated (unfindable) session",
          "[session_support][naming]") {
    // The rename persists even when the live object is already gone from the
    // registry -- the database is authoritative, and the notification is best
    // effort.
    SupportFixture fixture;
    auto session = fixture.CreateSession("");
    const auto id = session->session_id();
    fixture.registry->Terminate(id);
    session.reset();

    CHECK(fixture.support->RenameSession(id, "named after the fact"));
    CHECK(fixture.db->GetSession(id)->name == "named after the fact");
}

// --- the USER rename seam (SessionControl::RenameSession, /rename) ----------

TEST_CASE("a user rename persists as 'explicit' and notifies, unconditionally",
          "[session_support][naming]") {
    // Unlike the titler's seam, a USER rename clobbers anything -- including
    // a name the user chose at creation -- and stamps name_source='explicit',
    // which is what makes a later titler pass skip the session.
    SupportFixture fixture;
    auto session = fixture.CreateSession("chosen by the user");
    REQUIRE(session != nullptr);

    CHECK(session->RenameSession("my own tab name"));

    CHECK(fixture.db->GetSession(session->session_id())->name == "my own tab name");
    CHECK(fixture.db->GetSession(session->session_id())->name_source == "explicit");
    CHECK(session->info().name == "my own tab name");
    CHECK(QueueHasRename(*session, "my own tab name"));
}

TEST_CASE("a user rename over an observer-set name also lands as 'explicit'",
          "[session_support][naming]") {
    // The titler renamed first; the user overrides. After this the titler
    // must never fire again for the session -- IsExplicitlyNamed now reads
    // true, exactly the guard the titler's own seam consults.
    SupportFixture fixture;
    auto session = fixture.CreateSession("");
    REQUIRE(session != nullptr);
    REQUIRE(fixture.support->RenameSession(session->session_id(), "a generated title"));

    CHECK(session->RenameSession("user wins"));

    CHECK(fixture.db->GetSession(session->session_id())->name == "user wins");
    CHECK(fixture.db->GetSession(session->session_id())->name_source == "explicit");
    CHECK(fixture.db->IsExplicitlyNamed(session->session_id()));
}

TEST_CASE("a user rename of an unknown name is refused, and an empty one never "
          "reaches the database",
          "[session_support][naming]") {
    SupportFixture fixture;
    auto session = fixture.CreateSession("");
    REQUIRE(session != nullptr);

    CHECK_FALSE(session->RenameSession(""));
    CHECK(session->info().name.empty());
    CHECK(testing::DrainEvents(*session).empty());
}
