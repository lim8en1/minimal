// Live MCP smoke test against a REAL MCP server (`gateway`, an IDA Pro MCP
// gateway that speaks both stdio and streamable-http).
//
// This is a HARD REQUIREMENT on machines where MINIMAL_ENABLE_LIVE_MCP_TESTS
// is ON: a missing server is a FAIL, never a skip. CI never sets the option,
// so the test target is not even built there (CMakeLists.txt gates it).
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>

using namespace std::chrono_literals;

#include <catch2/catch_test_macros.hpp>

#include "minimal/common/logger.hpp"
#include "minimal/mcp/mcp_client_manager.hpp"
#include "minimal/tools/tool_registry.hpp"

using namespace minimal;

namespace {

constexpr auto kConnectTimeout = 15s;
constexpr auto kRequestTimeout = 30s;

// Discovers the gateway binary: explicit override first, then PATH.
std::string FindGateway() {
    if (const char* override_path = std::getenv("MINIMAL_MCP_LIVE_SERVER")) {
        if (!std::filesystem::exists(override_path)) {
            FAIL("MINIMAL_MCP_LIVE_SERVER points at a missing file: " << override_path);
        }
        return override_path;
    }
    const char* path_env = std::getenv("PATH");
    REQUIRE(path_env != nullptr);
    std::string paths = path_env;
    std::size_t start = 0;
    while (start <= paths.size()) {
        const std::size_t sep = paths.find(':', start);
        const std::string dir =
            paths.substr(start, sep == std::string::npos ? std::string::npos : sep - start);
        const std::string candidate = dir + "/gateway";
        if (!dir.empty() && std::filesystem::exists(candidate)) return candidate;
        if (sep == std::string::npos) break;
        start = sep + 1;
    }
    FAIL("gateway not found on PATH (install it, or set MINIMAL_MCP_LIVE_SERVER)");
    return "";
}

mcp::McpEndpoint StdioEndpoint(const std::string& command) {
    // Unique registration port per spawn: the gateway always starts a
    // registration listener, and two live tests running together (or a stray
    // server from an earlier run) would collide on the default.
    static int reg_port_counter = 0;
    mcp::McpEndpoint endpoint;
    endpoint.transport = mcp::McpTransport::Stdio;
    endpoint.command_line = command + " --transport stdio --reg-port " +
                            std::to_string(33500 + (reg_port_counter++ % 64));
    return endpoint;
}

mcp::McpEndpoint HttpEndpoint(int port) {
    mcp::McpEndpoint endpoint;
    endpoint.transport = mcp::McpTransport::Http;
    endpoint.url = "http://127.0.0.1:" + std::to_string(port) + "/mcp";
    return endpoint;
}

// Spawns the http gateway and reaps it on destruction -- a test that leaves a
// server bound to a fixed port would poison every later run.
class HttpGateway {
public:
    explicit HttpGateway(const std::string& gateway) {
        static int port_counter = 0;
        port_ = 19765 + (port_counter++ % 32);
        reg_port_ = 33700 + (port_ % 64);
        pid_ = ::fork();
        if (pid_ == 0) {
            const std::string port = std::to_string(port_);
            const std::string reg_port = std::to_string(reg_port_);
            ::execl(gateway.c_str(), gateway.c_str(), "--transport", "http", "--port",
                    port.c_str(), "--reg-port", reg_port.c_str(),
                    static_cast<char*>(nullptr));
            ::_exit(127);  // exec failed
        }
        REQUIRE(pid_ > 0);
    }

    ~HttpGateway() {
        if (pid_ > 0) {
            ::kill(pid_, SIGTERM);
            int status = 0;
            ::waitpid(pid_, &status, 0);
        }
    }

    int port() const { return port_; }

private:
    pid_t pid_ = -1;
    int port_ = 0;
    int reg_port_ = 0;
};

}  // namespace

TEST_CASE("live gateway over stdio: initialize, discovery, tool call", "[mcp_live]") {
    const std::string gateway = FindGateway();

    mcp::McpConnection::Failure error;
    auto connection = mcp::McpConnection::Open(
        StdioEndpoint(gateway), mcp::McpLimits{kConnectTimeout, kRequestTimeout},
        "minimal-live", "0.1.0", error);
    REQUIRE(connection != nullptr);

    CHECK(connection->Ping(nullptr, error));

    const auto tools = connection->ListTools(nullptr, error);
    REQUIRE(tools.has_value());
    // The tool surface is IDA-specific; a stable floor is the instance
    // management tool, present whether or not IDA is running.
    bool saw_list = false;
    for (const auto& tool : *tools) {
        if (tool.name == "list_instances") saw_list = true;
    }
    CHECK(saw_list);

    // Read-only and IDA-less-safe: returns an empty instance list.
    const mcp::McpCallOutcome outcome =
        connection->CallTool("list_instances", nlohmann::json::object(), nullptr);
    CHECK(outcome.ok);
    if (outcome.ok) {
        CHECK(outcome.result.value("isError", true) == false);
    }
}

TEST_CASE("live gateway over http: initialize, discovery, tool call", "[mcp_live]") {
    const std::string gateway = FindGateway();
    HttpGateway server(gateway);

    // The manager's Open is the readiness probe: retry until the endpoint
    // accepts, bounded by the gateway's own startup time.
    bool up = false;
    for (int attempt = 0; attempt < 40 && !up; ++attempt) {
        std::this_thread::sleep_for(250ms);
        mcp::McpConnection::Failure error;
        auto connection =
            mcp::McpConnection::Open(HttpEndpoint(server.port()),
                                     mcp::McpLimits{1s, kRequestTimeout}, "minimal-live",
                                     "0.1.0", error);
        if (!connection) continue;
        up = true;
        CHECK(connection->Ping(nullptr, error));
        const auto tools = connection->ListTools(nullptr, error);
        REQUIRE(tools.has_value());
        bool saw_list = false;
        for (const auto& tool : *tools) {
            if (tool.name == "list_instances") saw_list = true;
        }
        CHECK(saw_list);
        const mcp::McpCallOutcome outcome =
            connection->CallTool("list_instances", nlohmann::json::object(), nullptr);
        CHECK(outcome.ok);
    }
    CHECK(up);
}

TEST_CASE("live gateway through the manager: tools registered and executed", "[mcp_live]") {
    const std::string gateway = FindGateway();

    tools::ToolRegistry registry;
    const std::string config_json =
        nlohmann::json::array({nlohmann::json{
                                   {"name", "ida"},
                                   {"transport", "stdio"},
                                   {"command", gateway},
                                   {"args", {"--transport", "stdio", "--reg-port", "33900"}},
                                   {"enabled", true}}})
            .dump();
    const std::filesystem::path config_path =
        std::filesystem::temp_directory_path() /
        ("minimal_live_mcps_" + std::to_string(::getpid()) + ".json");
    { std::ofstream(config_path) << config_json; }

    mcp::McpClientManager manager(config::LoadMcpsConfig(config_path), registry,
                                  mcp::McpLimits{kConnectTimeout, kRequestTimeout},
                                  common::DefaultLogger());
    std::string error;
    REQUIRE(manager.EnsureAlive("ida", error));
    CHECK(manager.StatusOf("ida").alive);

    const auto defs = registry.DefinitionsForMcpServers({"ida"});
    CHECK_FALSE(defs.empty());
    bool saw_list = false;
    for (const auto& def : defs) {
        if (def.name == "mcp__ida__list_instances") saw_list = true;
    }
    CHECK(saw_list);

    // Execute through the registry path, exactly as a session worker would.
    auto* tool = registry.Find("mcp__ida__list_instances");
    REQUIRE(tool != nullptr);
    tools::ToolContext ctx{common::DefaultLogger()};
    ctx.session_id = "live";
    const tools::ToolResult result = tool->execute(nlohmann::json::object(), ctx);
    CHECK_FALSE(result.is_error);
    CHECK(result.content_text.find("instances") != std::string::npos);

    std::error_code ec;
    std::filesystem::remove(config_path, ec);
}
