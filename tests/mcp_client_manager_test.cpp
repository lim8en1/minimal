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
#include "minimal/tools/tool_registry.hpp"

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
    auto* marker_tool = fixture.registry.Find("mcp__fake__getenv");
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
}

TEST_CASE("McpClientManager: server names that cannot be namespaced are refused", "[mcp_manager]") {
    Fixture fixture(nlohmann::json::array({StdioEntry("bad__name", "normal")}));

    std::string error = "untouched";
    CHECK_FALSE(fixture.manager->EnsureAlive("bad__name", error));
    CHECK(error.find("cannot be namespaced") != std::string::npos);
    CHECK(fixture.registry.DefinitionsForMcpServers({"broken"}).empty());
}
