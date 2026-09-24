// Client auto-spawn: connect-or-start for the local server.
//
// Tier 1 (pure) covers the target classification and binary resolution; tier 4
// runs the REAL lifecycle against the REAL minimal-server binary: spawn, serve,
// then the idle reaper's self-exit -- the property the whole design leans on
// ("a client-spawned server cleans itself up", ARCHITECTURE.md Distribution).
// The binary path arrives from CMake as a compile definition, the same way the
// MCP fake server does.
//
// ConfigDir/DataDir resolve from env, and the forked server inherits the test
// process's environment -- so setenv BEFORE the spawn redirects the spawned
// server's config, data and log into the temp dir. catch_discover_tests runs
// each TEST_CASE as its own process, so the env writes stay contained.

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "minimal/rpc/session_wiring.hpp"
#include "minimal/agent/agent_preset_registry.hpp"
#include "minimal/agent/session_observer_registry.hpp"
#include "minimal/agent/session_registry.hpp"
#include "minimal/client/server_spawn.hpp"
#include "minimal/client/session_client.hpp"
#include "minimal/common/config_paths.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/config/settings_config.hpp"
#include "minimal/config/skill_registry.hpp"
#include "minimal/llm/provider_factory_registry.hpp"
#include "minimal/rpc/server.hpp"
#include "minimal/rpc/session_service.hpp"
#include "minimal/session/session_database.hpp"
#include "minimal/tools/tool_registry.hpp"
#include "temp_dir.hpp"

using namespace std::chrono_literals;
using namespace minimal;

using minimal::client::EnsureServerRunning;
using minimal::client::IsSpawnableTarget;
using minimal::client::LocateServerBinary;
using minimal::client::SpawnOptions;

namespace {

#ifndef MINIMAL_SPAWN_TEST_SERVER
#error "MINIMAL_SPAWN_TEST_SERVER must name the built minimal-server binary"
#endif

void SeedConfig(const std::filesystem::path& dir) {
    std::filesystem::create_directories(dir / "agents");
    const auto write = [&dir](const std::filesystem::path& relative, const std::string& body) {
        std::ofstream(dir / relative) << body;
    };
    // The sanity check's floor: one provider, one preset naming it. The provider
    // never has to answer -- the turn tests live elsewhere; this server only has
    // to come up, listen, and idle out.
    write("providers.json",
          R"({"providers": {"main": {"backend": "openai", "url": "http://127.0.0.1:9/v1",)"
          R"( "api_key": "test", "models": {"test-model": {}}}}})");
    write("settings.json", R"({"default_agent_preset": "main"})");
    write("permissions.json", R"({"fallback": {"execute": "ask"}})");
    write("agents/main.md",
          "---\nname: main\nmodel: main/test-model\n---\nYou help.\n");
}

}  // namespace

TEST_CASE("spawnable targets are unix sockets, nothing else", "[server_spawn]") {
    CHECK(IsSpawnableTarget("unix:///tmp/x.sock"));
    CHECK(IsSpawnableTarget("unix://" + (minimal::common::DataDir() / "server.sock").string()));
    CHECK_FALSE(IsSpawnableTarget(""));
    CHECK_FALSE(IsSpawnableTarget("localhost:9000"));
    CHECK_FALSE(IsSpawnableTarget("127.0.0.1:9000"));
    CHECK_FALSE(IsSpawnableTarget("dns:///example.internal:9000"));
    CHECK_FALSE(IsSpawnableTarget("unix-abstract://minimall"));
}

TEST_CASE("LocateServerBinary honours MINIMAL_SERVER_BIN", "[server_spawn]") {
    minimal::testing::TempDir dir;
    const auto script = dir.path() / "fake-server";
    {
        std::ofstream out(script);
        out << "#!/bin/sh\nexit 0\n";
    }
    std::filesystem::permissions(script, std::filesystem::perms::owner_exec);

    ::setenv("MINIMAL_SERVER_BIN", script.string().c_str(), /*overwrite=*/1);
    CHECK(LocateServerBinary() == script.string());
    ::unsetenv("MINIMAL_SERVER_BIN");
}

TEST_CASE("EnsureServerRunning with spawn disabled fails fast on a dead target",
          "[server_spawn]") {
    minimal::testing::TempDir dir;
    const std::string target = "unix://" + (dir.path() / "nobody.sock").string();

    SpawnOptions options;
    options.enabled = false;
    const auto started = std::chrono::steady_clock::now();
    CHECK_FALSE(EnsureServerRunning(target, options));
    // No probe of the STARTUP timeout: the failure must be near-instant, not a
    // polite 25 s stall in every script that opted out.
    CHECK(std::chrono::steady_clock::now() - started < 10s);
}

TEST_CASE("EnsureServerRunning recognises an already-running server without spawning",
          "[server_spawn][e2e]") {
    // The no-spawn happy path, against a real listener: an in-process
    // rpc::Server plays the role of the server the user started by hand.
    minimal::testing::TempDir dir;
    const auto socket = dir.path() / "already.sock";
    const std::string target = "unix://" + socket.string();

    // A service needs a registry; the smallest real one serves nothing here --
    // only the transport has to come up for WaitForConnected. SeedConfig gives
    // the preset registry its floor (one agents/*.md), like every other fixture.
    SeedConfig(dir.path());
    const auto skills = config::SkillRegistry::Load(dir.path());
    tools::ToolRegistry tools;
    llm::ProviderFactoryRegistry providers;
    agent::SessionObserverRegistry observers{common::DefaultLogger()};
    session::SessionDatabase db{dir.path() / "sessions.db"};
    config::SettingsConfig settings;
    agent::AgentPresetRegistry presets{dir.path(), skills, settings};
    agent::SessionRegistry registry{presets, rpc::FinishSessionWiring,
        plugins::HostRegistries{.tools = &tools,
                                .providers = &providers,
                                .observers = &observers},
        db, agent::LoopConfig{}, common::DefaultLogger()};
    rpc::SessionServiceImpl service{registry, db, settings, config::PermissionsConfig{},
                                    common::DefaultLogger()};
    rpc::Server server{service, socket};
    REQUIRE(server.listening());

    SpawnOptions options;  // enabled: the assertion is that NO spawn happens
    REQUIRE(EnsureServerRunning(target, options));

    client::SessionClient probe{target};
    CHECK(probe.WaitForConnected(5s));
}

TEST_CASE("a client with no server spawns one, uses it, and the server exits on its own",
          "[server_spawn][e2e]") {
    minimal::testing::TempDir dir;
    const auto config_dir = dir.path() / "config";
    SeedConfig(config_dir);
    ::setenv("MINIMAL_CONFIG_DIR", config_dir.string().c_str(), 1);
    ::setenv("XDG_DATA_HOME", dir.path().string().c_str(), 1);
    ::setenv("MINIMAL_SERVER_BIN", MINIMAL_SPAWN_TEST_SERVER, 1);

    const std::string target =
        "unix://" + (minimal::common::DataDir() / "spawn-test.sock").string();
    const auto socket_path = minimal::common::DataDir() / "spawn-test.sock";

    std::vector<std::string> progress;
    SpawnOptions options;
    // Surface every progress line with the failure: a spawn probe that times
    // out is diagnosed by the LINES IT PRINTED (which startup step stalled),
    // and the test's TempDir -- with the server's own spawn log -- is already
    // gone by the time the failure is read.
    const bool up = EnsureServerRunning(target, options,
                                        [&](const std::string& line) { progress.push_back(line); });
    for (const auto& line : progress) UNSCOPED_INFO("progress: " << line);
    if (!up) {
        // The spawned server's own diagnostics: it logs startup, bind and
        // sanity-check failures to server-spawn.log under the data dir. Copy
        // the tail into the report before the TempDir's destructor deletes it.
        std::ifstream spawn_log(minimal::client::SpawnedServerLogPath());
        if (spawn_log) {
            std::string line;
            while (std::getline(spawn_log, line)) UNSCOPED_INFO("server: " << line);
        } else {
            UNSCOPED_INFO("no server-spawn log at " << minimal::client::SpawnedServerLogPath());
        }
    }
    REQUIRE(up);

    // Listening: a real client reaches it. (The socket file alone would not
    // prove serving -- it exists exactly once the bind happened, but the probe
    // inside EnsureServerRunning already asserts the transport, so this is the
    // RPC layer coming up behind it.)
    client::SessionClient client{target};
    REQUIRE(client.WaitForConnected(5s));
    // Empty project_root requires include_all_dirs (the server refuses the
    // empty-root listing by design).
    client::SessionClient::ListSessionsOptions list_options;
    list_options.include_all_dirs = true;
    CHECK(client.ListSessions(list_options).status.ok());
    CHECK(std::filesystem::exists(socket_path));

    // The client goes away; nothing holds the server. idle_shutdown_seconds
    // defaults to 5, so the reaper collects it -- the self-cleanup half of the
    // contract. The socket file's removal in ~Server is the observable.
    bool exited = false;
    const auto deadline = std::chrono::steady_clock::now() + 60s;
    while (std::chrono::steady_clock::now() < deadline) {
        std::error_code ec;
        if (!std::filesystem::exists(socket_path, ec)) {
            exited = true;
            break;
        }
        std::this_thread::sleep_for(200ms);
    }
    CHECK(exited);

    // And the diagnostics land where the failure path says they do.
    CHECK(std::filesystem::exists(minimal::client::SpawnedServerLogPath()));

    ::unsetenv("MINIMAL_SERVER_BIN");
    ::unsetenv("XDG_DATA_HOME");
    ::unsetenv("MINIMAL_CONFIG_DIR");
}
