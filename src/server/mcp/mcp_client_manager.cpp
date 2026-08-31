#include "minimal/mcp/mcp_client_manager.hpp"

#include <cstdlib>
#include <optional>
#include <utility>

#include "minimal/common/logger.hpp"
#include "minimal/mcp/env_interpolation.hpp"
#include "minimal/mcp/naming.hpp"

namespace minimal::mcp {

// Per-server shared state. The invoker handed to McpTool holds a weak_ptr to
// the handle and reads `connection` under the manager's mutex, so a respawn
// (which swaps the connection) is atomic from a tool call's point of view --
// and an in-flight call keeps the OLD connection alive through its own
// shared_ptr copy.
struct McpClientManager::ServerHandle {
    config::McpServerEntry entry;
    std::shared_ptr<HandleInvoker> invoker;
    mutable std::mutex mu;  // guards connection/alive/generation swaps (StatusOf is const)
    std::shared_ptr<McpConnection> connection;
    bool alive = false;
    uint64_t generation = 0;
    std::string error;
};

// The McpInvoker implementation McpTool carries. Weak to the handle: if the
// manager itself is gone (teardown while a session worker still runs a tool),
// the call degrades to a clean error instead of crashing.
class McpClientManager::HandleInvoker final : public McpInvoker {
public:
    explicit HandleInvoker(std::weak_ptr<ServerHandle> handle) : handle_(std::move(handle)) {}

    McpCallOutcome CallTool(std::string_view tool_name, const nlohmann::json& arguments,
                            const std::function<bool()>& is_cancelled) override {
        std::shared_ptr<ServerHandle> handle = handle_.lock();
        if (handle == nullptr) {
            return {false, "MCP server is no longer configured", nlohmann::json()};
        }
        std::shared_ptr<McpConnection> connection;
        {
            std::lock_guard<std::mutex> lock(handle->mu);
            connection = handle->connection;
            if (!handle->alive || !connection) {
                return {false, "MCP server '" + handle->entry.name + "' is not connected",
                        nlohmann::json()};
            }
        }
        return connection->CallTool(tool_name, arguments, is_cancelled);
    }

private:
    std::weak_ptr<ServerHandle> handle_;
};

McpClientManager::McpClientManager(config::McpsConfig config, tools::ToolRegistry& registry,
                                   const McpLimits& limits, common::Logger& logger)
    : config_(std::move(config)), registry_(registry), limits_(limits), logger_(logger) {
    for (const auto& entry : config_.entries) {
        if (!entry.enabled) continue;  // deliberate opt-out, silent
        auto handle = std::make_shared<ServerHandle>();
        handle->entry = entry;
        handle->invoker = std::make_shared<HandleInvoker>(handle);
        handles_.emplace(entry.name, std::move(handle));
    }
}

namespace {

std::optional<config::McpServerEntry> ResolveEndpoint(const config::McpServerEntry& entry,
                                                      std::string& error) {
    // ${ENV_VAR} interpolation happens at connect time, never logged, per
    // mcps_config.hpp. An unresolvable marker is a connect failure naming the
    // ENTRY -- never the value.
    config::McpServerEntry resolved = entry;
    auto lookup = [](std::string_view name) -> std::optional<std::string> {
        const char* value = std::getenv(std::string(name).c_str());
        if (value == nullptr) return std::nullopt;
        return std::string(value);
    };
    for (auto& [key, value] : resolved.env) {
        std::string marker;
        if (auto interpolated = InterpolateEnv(value, lookup, &marker)) {
            value = *std::move(interpolated);
        } else {
            error = "MCP server '" + entry.name + "': env '" + key +
                    "' references an unset variable '" + marker + "'";
            return std::nullopt;
        }
    }
    for (auto& [key, value] : resolved.headers) {
        std::string marker;
        if (auto interpolated = InterpolateEnv(value, lookup, &marker)) {
            value = *std::move(interpolated);
        } else {
            error = "MCP server '" + entry.name + "': header '" + key +
                    "' references an unset variable '" + marker + "'";
            return std::nullopt;
        }
    }
    return resolved;
}

McpEndpoint ToEndpoint(const config::McpServerEntry& entry) {
    McpEndpoint endpoint;
    endpoint.transport = entry.transport == config::McpTransport::Stdio ? McpTransport::Stdio
                                                                        : McpTransport::Http;
    // cpp-mcp splits the command line on whitespace: command plus args joined.
    // Args containing spaces cannot cross this boundary (recorded limitation).
    std::string command_line = entry.command;
    for (const auto& arg : entry.args) {
        command_line += " " + arg;
    }
    endpoint.command_line = std::move(command_line);
    endpoint.url = entry.url;
    endpoint.env = entry.env;
    endpoint.headers = entry.headers;
    return endpoint;
}

}  // namespace

bool McpClientManager::StartHandleLocked(const config::McpServerEntry& entry,
                                         ServerHandle& handle, std::string& error) {
    // Close any previous connection first: a dead child must not linger, and
    // the swap must be atomic from the invoker's point of view (a concurrent
    // CallTool holds the old connection alive through its shared_ptr copy).
    if (handle.connection) {
        std::lock_guard<std::mutex> lock(handle.mu);
        handle.connection.reset();
        handle.alive = false;
    }

    const auto resolved = ResolveEndpoint(entry, error);
    if (!resolved) return false;

    McpConnection::Failure failure;
    auto connection =
        McpConnection::Open(ToEndpoint(*resolved), limits_, "minimal-server", "0.1.0", failure);
    if (!connection) {
        error = "MCP server '" + entry.name + "' failed to connect: " + failure.message;
        return false;
    }

    McpConnection::Failure tools_error;
    const auto listings = connection->ListTools(nullptr, tools_error);
    if (!listings) {
        error = "MCP server '" + entry.name + "' tool discovery failed: " + tools_error.message;
        return false;
    }

    // Validate the namespace BEFORE registering: a server (or config) name
    // that would make mcp__<server>__<tool> ambiguous skips registration with
    // a named reason rather than corrupting the wire namespace.
    if (!IsValidMcpServerName(entry.name)) {
        error = "MCP server name '" + entry.name +
                "' cannot be namespaced (contains '__', whitespace or control characters)";
        return false;
    }

    auto invoker = handle.invoker;
    std::vector<std::unique_ptr<tools::Tool>> tools;
    std::size_t registered = 0;
    for (const auto& listing : *listings) {
        if (!IsValidMcpToolName(listing.name)) {
            logger_.warn("MCP server '{}' tool '{}' skipped: name contains '__', whitespace or "
                         "control characters",
                         entry.name, listing.name);
            continue;
        }
        llm::ToolDefinition definition;
        definition.name = listing.name;
        definition.description = listing.description;
        definition.input_schema = listing.input_schema;
        // Each tool takes its OWN shared_ptr copy -- moving would null the
        // invoker for every tool after the first (exactly what the env-marker
        // round-trip test caught).
        tools.push_back(
            std::make_unique<McpTool>(entry.name, std::move(definition), invoker));
        ++registered;
    }
    registry_.ReplaceMcpTools(entry.name, std::move(tools));

    {
        std::lock_guard<std::mutex> lock(handle.mu);
        handle.connection = std::move(connection);
        handle.alive = true;
        ++handle.generation;
        handle.error.clear();
    }
    logger_.info("MCP server '{}': connected, {} tools registered", entry.name, registered);
    return true;
}

bool McpClientManager::EnsureAlive(const std::string& server_name, std::string& error) {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = handles_.find(server_name);
    if (it == handles_.end()) {
        error = "MCP server '" + server_name + "' is not configured (mcps.json) or is disabled";
        return false;
    }
    ServerHandle& handle = *it->second;

    if (handle.alive && handle.connection) {
        // The liveness check the architecture asks for at session start.
        McpConnection::Failure ping_error;
        if (handle.connection->Ping(nullptr, ping_error)) return true;
        logger_.warn("MCP server '{}' liveness check failed ({}); restarting", server_name,
                     ping_error.message);
    }

    if (StartHandleLocked(handle.entry, handle, error)) return true;
    {
        std::lock_guard<std::mutex> lock(handle.mu);
        handle.alive = false;
        handle.error = error;
    }
    logger_.error("MCP server '{}' unavailable: {}", server_name, error);
    return false;
}

McpClientManager::Status McpClientManager::StatusOf(const std::string& server_name) const {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = handles_.find(server_name);
    if (it == handles_.end()) return {};
    const ServerHandle& handle = *it->second;
    {
        std::lock_guard<std::mutex> handle_lock(handle.mu);
        Status status;
        status.configured = true;
        status.alive = handle.alive;
        status.generation = handle.generation;
        status.error = handle.error;
        return status;
    }
}

std::vector<std::string> McpClientManager::ServerNames() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<std::string> names;
    names.reserve(handles_.size());
    for (const auto& [name, handle] : handles_) names.push_back(name);
    return names;
}

bool McpClientManager::IsConfigured(const std::string& server_name) const {
    std::lock_guard<std::mutex> lock(mu_);
    return handles_.count(server_name) != 0;
}

}  // namespace minimal::mcp
