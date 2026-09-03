#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <unistd.h>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "minimal/common/logger.hpp"
#include "minimal/mcp/mcp_client_manager.hpp"
#include "minimal/tools/tool.hpp"
#include "minimal/tools/tool_registry.hpp"
#include "support/deadline.hpp"
#include "support/fake_http_mcp_server.hpp"

using minimal::config::LoadMcpsConfig;
using minimal::mcp::McpClientManager;
using minimal::mcp::McpLimits;
using minimal::tools::ToolRegistry;

namespace {

constexpr const char* kFakeServer = MINIMAL_MCP_FAKE_SERVER;

std::string WriteMcpsConfig(const std::string& json) {
    const std::string path =
        "/tmp/minimal_mcp_manager_test_" + std::to_string(getpid()) + "_" +
        std::to_string(static_cast<long>(std::chrono::steady_clock::now().time_since_epoch().count() %
                                         1000000)) +
        ".json";
    auto file = std::ofstream(path);
    file << json;
    return path;
}

nlohmann::json StdioEntry(const std::string& name, const std::string& mode) {
    return {{"name", name},
            {"transport", "stdio"},
            {"command", kFakeServer},
            {"args", {mode}},
            {"enabled", true}};
}

struct Fixture {
    ToolRegistry registry;
    std::unique_ptr<McpClientManager> manager;

    explicit Fixture(const nlohmann::json& entries,
                     McpLimits timeouts = {std::chrono::seconds(10), std::chrono::seconds(10)}) {
        const std::string path = WriteMcpsConfig(entries.dump());
        manager = std::make_unique<McpClientManager>(LoadMcpsConfig(path), registry, timeouts,
                                                     minimal::common::DefaultLogger());
        std::remove(path.c_str());
    }
};

}  // namespace

TEST_CASE("McpClientManager: one shared connection, discovered tools registered", "[mcp_manager]") {
    Fixture fixture(nlohmann::json::array({StdioEntry("fake", "normal")}));

    std::string error;
    REQUIRE(fixture.manager->EnsureAlive("fake", error));

    const auto tools = fixture.registry.DefinitionsForMcpServers({"fake"});
    REQUIRE(tools.size() == 4);
    CHECK(tools[0].name == "mcp__fake__echo");
    CHECK(tools[0].input_schema.is_object());

    const auto status = fixture.manager->StatusOf("fake");
    CHECK(status.configured);
    CHECK(status.alive);
    CHECK(status.generation == 1);
}

TEST_CASE("McpClientManager: repeated liveness checks do not respawn", "[mcp_manager]") {
    Fixture fixture(nlohmann::json::array({StdioEntry("fake", "normal")}));

    std::string error;
    REQUIRE(fixture.manager->EnsureAlive("fake", error));
    const auto first = fixture.manager->StatusOf("fake");
    REQUIRE(fixture.manager->EnsureAlive("fake", error));
    REQUIRE(fixture.manager->EnsureAlive("fake", error));
    const auto second = fixture.manager->StatusOf("fake");
    CHECK(first.generation == second.generation);  // one spawn, cached discovery
}

TEST_CASE("McpClientManager: a restarted server respawns and re-discovers", "[mcp_manager]") {
    Fixture fixture(nlohmann::json::array({StdioEntry("fake", "die_after_ms:2000")}));

    std::string error;
    REQUIRE(fixture.manager->EnsureAlive("fake", error));
    const auto before = fixture.manager->StatusOf("fake");
    CHECK(before.alive);

    // Outlive the scripted death, then check liveness again: the manager must
    // respawn (new generation) and the tools must keep working.
    std::this_thread::sleep_for(std::chrono::seconds(3));
    REQUIRE(fixture.manager->EnsureAlive("fake", error));
    const auto after = fixture.manager->StatusOf("fake");
    CHECK(after.generation == before.generation + 1);

    const auto tools = fixture.registry.DefinitionsForMcpServers({"fake"});
    CHECK(tools.size() == 4);  // re-registered, not duplicated
}

TEST_CASE("McpClientManager: a restart does not stall behind the old connection's teardown",
          "[mcp_manager]") {
    // "stubborn" ignores SIGTERM: tearing its connection down inline would
    // need the full ~5 s SIGTERM-grace-then-SIGKILL reap (mcp_stdio_shim.cpp).
    // A short request timeout means the liveness ping queued behind the
    // in-flight "hang" call times out quickly, giving EnsureAlive a
    // deterministic reason to restart without waiting on the hang call itself.
    Fixture fixture(nlohmann::json::array({StdioEntry("fake", "stubborn")}),
                    {std::chrono::seconds(5), std::chrono::milliseconds(500)});
    std::string error;
    REQUIRE(fixture.manager->EnsureAlive("fake", error));

    auto hang_tool = fixture.registry.Find("mcp__fake__hang");
    REQUIRE(hang_tool != nullptr);
    minimal::tools::ToolContext ctx(minimal::common::DefaultLogger());
    // Occupies the connection's one worker thread for effectively ever (the
    // fake server's hang tool sleeps 10 minutes); this call itself returns in
    // ~500 ms once ITS OWN request times out, leaving the worker still stuck.
    std::thread occupy_worker(
        [hang_tool, ctx]() mutable { (void)hang_tool->execute(nlohmann::json::object(), ctx); });
    occupy_worker.detach();

    // Past kVerifiedTtl (2 s, mcp_client_manager.cpp) plus enough slack for
    // the hang call above to have actually started running on the worker.
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));

    const auto started = std::chrono::steady_clock::now();
    REQUIRE(fixture.manager->EnsureAlive("fake", error));  // ping times out -> restart
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK(fixture.manager->StatusOf("fake").generation == 2);

    // Bounded by the ping timeout (500 ms) plus a fresh spawn+handshake --
    // nowhere near the old (stubborn, wedged) connection's multi-second
    // teardown, which the graveyard reaper now pays instead of this call.
    CHECK(elapsed < std::chrono::seconds(3));
}

TEST_CASE("McpClientManager: a failed restart is rate-limited by the cooldown", "[mcp_manager]") {
    // "garbage" never completes the handshake: every restart attempt against
    // it fails, at the cost of a full connect-timeout wait. Without a
    // cooldown, a subagent spawn loop against a down server pays that cost
    // once per spawn, at spawn rate.
    Fixture fixture(nlohmann::json::array({StdioEntry("broken", "garbage")}),
                    {std::chrono::seconds(2), std::chrono::seconds(2)});
    std::string error;
    CHECK_FALSE(fixture.manager->EnsureAlive("broken", error));
    CHECK(error.find("cooldown") == std::string::npos);  // the real failure, not yet rate-limited

    // Immediately retrying must not pay another full attempt: the default
    // restart_cooldown_ms (30 s) refuses it fast with a distinct reason.
    const auto started = std::chrono::steady_clock::now();
    CHECK_FALSE(fixture.manager->EnsureAlive("broken", error));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK(error.find("cooldown") != std::string::npos);
    CHECK(elapsed < std::chrono::milliseconds(500));  // nowhere near a real connect attempt
}

TEST_CASE("McpClientManager: a tool call respawns a server that died mid-session",
          "[mcp_manager]") {
    // No EnsureAlive between the death and the calls below -- handle.alive
    // stays stale-true until something actually notices, which is the point:
    // the FIRST call after the death is the one that discovers it (fails,
    // transport-level) and the NEXT one is what used to require a new session
    // or an /agent switch to get, per the (until now false) header comment.
    Fixture fixture(nlohmann::json::array({StdioEntry("fake", "die_after_ms:2000")}));
    std::string error;
    REQUIRE(fixture.manager->EnsureAlive("fake", error));
    const auto before = fixture.manager->StatusOf("fake");

    std::this_thread::sleep_for(std::chrono::seconds(3));  // outlive the scripted death

    auto echo_tool = fixture.registry.Find("mcp__fake__echo");
    REQUIRE(echo_tool != nullptr);
    minimal::tools::ToolContext ctx(minimal::common::DefaultLogger());

    const auto first = echo_tool->execute(nlohmann::json{{"text", "hi"}}, ctx);
    CHECK(first.is_error);  // the dead-server call itself still fails

    const auto second = echo_tool->execute(nlohmann::json{{"text", "hi"}}, ctx);
    CHECK_FALSE(second.is_error);  // respawned by the call above, this one works
    CHECK(second.content_text == "hi");
    CHECK(fixture.manager->StatusOf("fake").generation == before.generation + 1);
}

TEST_CASE("McpClientManager: a server that cannot start is a surfaced error", "[mcp_manager]") {
    // Garbage protocol output: the handshake never completes.
    Fixture fixture(nlohmann::json::array({StdioEntry("broken", "garbage")}),
                    {std::chrono::seconds(2), std::chrono::seconds(5)});

    std::string error = "untouched";
    CHECK_FALSE(fixture.manager->EnsureAlive("broken", error));
    CHECK(error != "untouched");
    CHECK_FALSE(fixture.manager->StatusOf("broken").alive);
    CHECK(fixture.registry.DefinitionsForMcpServers({"broken"}).empty());
}

TEST_CASE("McpClientManager: unconfigured and disabled servers are not alive", "[mcp_manager]") {
    nlohmann::json disabled = StdioEntry("off", "normal");
    disabled["enabled"] = false;
    Fixture fixture(nlohmann::json::array({disabled, StdioEntry("on", "normal")}));

    std::string error;
    CHECK_FALSE(fixture.manager->EnsureAlive("missing", error));
    CHECK(error.find("not configured") != std::string::npos);
    CHECK_FALSE(fixture.manager->EnsureAlive("off", error));
    CHECK(fixture.manager->StatusOf("missing").configured == false);
    CHECK(fixture.manager->IsConfigured("on"));
    CHECK_FALSE(fixture.manager->IsConfigured("off"));
}

TEST_CASE("McpClientManager: ${ENV_VAR} interpolation reaches the child and unset vars fail", "[mcp_manager]") {
    // The fake server's getenv tool echoes the child's env, proving the
    // interpolated value crossed the spawn boundary.
    nlohmann::json entry = StdioEntry("fake", "normal");
    entry["env"] = {{"MCP_FAKE_MARKER", "${MINIMAL_TEST_MARKER_VAR}"}};
    Fixture fixture(nlohmann::json::array({entry}));

    std::string error;
    ::setenv("MINIMAL_TEST_MARKER_VAR", "interpolated-value-123", 1);
    REQUIRE(fixture.manager->EnsureAlive("fake", error));

    // Prove the interpolated value reached the child: call the fake server's
    // getenv tool through the registered McpTool and read back the env value.
    auto marker_tool = fixture.registry.Find("mcp__fake__getenv");
    REQUIRE(marker_tool != nullptr);
    minimal::tools::ToolContext ctx{minimal::common::DefaultLogger()};
    const auto result = marker_tool->execute(nlohmann::json{{"name", "MCP_FAKE_MARKER"}}, ctx);
    CHECK_FALSE(result.is_error);
    CHECK(result.content_text == "interpolated-value-123");

    SECTION("unset variable is a connect failure naming the entry, not a silent empty") {
        std::string error2;
        ::unsetenv("MINIMAL_TEST_MARKER_VAR");
        // Respawn by liveness: the manager re-resolves interpolation on reconnect.
        // (The running connection stays alive, so force the error path through a
        // fresh manager instead.)
        ToolRegistry fresh_registry;
        const std::string path = WriteMcpsConfig(nlohmann::json::array({entry}).dump());
        McpClientManager fresh(LoadMcpsConfig(path), fresh_registry,
                               {std::chrono::seconds(5), std::chrono::seconds(5)},
                               minimal::common::DefaultLogger());
        std::string connect_error = "untouched";
        CHECK_FALSE(fresh.EnsureAlive("fake", connect_error));
        CHECK(connect_error.find("MINIMAL_TEST_MARKER_VAR") != std::string::npos);
        std::remove(path.c_str());
    }

    SECTION("a set-but-empty variable is treated the same as unset, not resolved to \"\"") {
        // Phase 3: env-lookup semantics unified onto "empty = unset" everywhere
        // (providers.json's api_key_env and $HOME/$XDG_* already worked this
        // way). A set-but-empty MCP env var used to resolve silently; it must
        // now fail the same way an unset one does.
        ::setenv("MINIMAL_TEST_MARKER_VAR", "", 1);
        ToolRegistry fresh_registry;
        const std::string path = WriteMcpsConfig(nlohmann::json::array({entry}).dump());
        McpClientManager fresh(LoadMcpsConfig(path), fresh_registry,
                               {std::chrono::seconds(5), std::chrono::seconds(5)},
                               minimal::common::DefaultLogger());
        std::string connect_error = "untouched";
        CHECK_FALSE(fresh.EnsureAlive("fake", connect_error));
        CHECK(connect_error.find("MINIMAL_TEST_MARKER_VAR") != std::string::npos);
        std::remove(path.c_str());
        ::unsetenv("MINIMAL_TEST_MARKER_VAR");
    }
}

TEST_CASE("McpClientManager: server names that cannot be namespaced are refused", "[mcp_manager]") {
    Fixture fixture(nlohmann::json::array({StdioEntry("bad__name", "normal")}));

    std::string error = "untouched";
    CHECK_FALSE(fixture.manager->EnsureAlive("bad__name", error));
    CHECK(error.find("cannot be namespaced") != std::string::npos);
    CHECK(fixture.registry.DefinitionsForMcpServers({"broken"}).empty());
}

TEST_CASE("McpClientManager: a fresh verdict skips the wire ping", "[mcp_manager]") {
    // The request count is the witness: connect+discover cost 3 requests
    // (initialize, initialized, tools/list); a follow-up EnsureAlive inside
    // the verified-TTL window must NOT add a ping request.
    FakeHttpMcpServer server;
    nlohmann::json entry = {{"name", "counted"},
                            {"transport", "http"},
                            {"url", server.url()},
                            {"enabled", true}};
    Fixture fixture(nlohmann::json::array({entry}));

    std::string error;
    REQUIRE(fixture.manager->EnsureAlive("counted", error));
    REQUIRE(server.requests() == 3);
    // Back-to-back: the second check rides the fresh verdict -- no ping
    // request reaches the server.
    REQUIRE(fixture.manager->EnsureAlive("counted", error));
    CHECK(server.requests() == 3);
    // After the TTL the check goes back to the wire: one more request (the
    // ping) and still no respawn.
    std::this_thread::sleep_for(std::chrono::seconds(3));
    REQUIRE(fixture.manager->EnsureAlive("counted", error));
    CHECK(server.requests() == 4);
    CHECK(fixture.manager->StatusOf("counted").generation == 1);
}

TEST_CASE("McpClientManager: one slow server does not block another's check", "[mcp_manager]") {
    // The gated http server parks its initialize until released; while it
    // holds, EnsureAlive for a DIFFERENT server must complete -- the
    // manager-wide lock covers only the map lookup, never network I/O.
    FakeHttpMcpServer gated(FakeHttpMcpServer::Options{.gate_initialize = true});
    nlohmann::json gated_entry = {{"name", "gated"},
                                  {"transport", "http"},
                                  {"url", gated.url()},
                                  {"enabled", true}};
    Fixture fixture(nlohmann::json::array({gated_entry, StdioEntry("fast", "normal")}),
                    {std::chrono::seconds(5), std::chrono::seconds(5)});

    const auto gated_run = minimal::testing::RunInBackground([&] {
        std::string error;
        return fixture.manager->EnsureAlive("gated", error);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));  // let it park on the gate

    std::string error;
    CHECK(fixture.manager->EnsureAlive("fast", error));  // must not wait behind 'gated'

    gated.ReleaseGate();
    CHECK(gated_run.WaitFor(std::chrono::seconds(5)));
    if (gated_run.Ready()) CHECK(gated_run.Get());
}
