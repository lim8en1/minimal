#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "minimal/config/mcps_config.hpp"
#include "minimal/mcp/mcp_connection.hpp"
#include "minimal/tools/tool_registry.hpp"

namespace minimal::common {
class Logger;
}

namespace minimal::mcp {

// Owns the shared MCP server processes: ONE connection per enabled mcps.json
// entry, shared across every session (the low-footprint goal -- not one child
// process per session). Liveness is checked when a SESSION starts with a preset
// listing the server (or /agent switches to one), never at server boot; tool
// discovery happens once per server process and is cached on its handle.
// Re-discovery happens only when a server process restarts.
//
// This is a core mechanism, not a plugin: sessions' ToolExecutor, agent loop
// and subagent gating all consult it.
class McpClientManager {
public:
    // `config` is the parsed mcps.json; disabled entries and entries with
    // recorded parse errors are skipped. `registry` receives discovered tools
    // under mcp__<server>__<tool>. Must outlive every session that was handed
    // tools from it (sessions borrow the manager via the registry's tools).
    McpClientManager(config::McpsConfig config, tools::ToolRegistry& registry,
                     const McpLimits& limits, common::Logger& logger);

    struct Status {
        bool configured = false;  // present and enabled in mcps.json
        bool alive = false;       // a live connection exists right now
        uint64_t generation = 0;  // bumped on every (re)connect; observable respawn
        std::string error;        // last failure, when !alive
    };

    // Liveness check at session start / /agent switch. Ensures the shared
    // connection exists and pings it; on failure the server is marked dead and
    // the error is surfaced (value, not exception) so the session can proceed
    // without it. A dead server is RESPAWNED here -- re-discovery is exactly
    // what a server process restart means.
    //
    // Blocking shape: the manager-wide lock covers only the handle lookup;
    // the ping/restart I/O runs under a per-server mutex, so slow servers
    // stall only their own callers. A verdict younger than ~10 s is reused
    // without a wire round trip.
    bool EnsureAlive(const std::string& server_name, std::string& error);

    Status StatusOf(const std::string& server_name) const;

    std::vector<std::string> ServerNames() const;
    bool IsConfigured(const std::string& server_name) const;

private:
    struct ServerHandle;
    class HandleInvoker;

    // Spawns/connects, initializes, discovers, and (re)registers the server's
    // tools into the registry. Returns false with `error` set. Caller holds
    // the handle's start_mu.
    bool StartHandle(const config::McpServerEntry& entry, ServerHandle& handle,
                     std::string& error);

    config::McpsConfig config_;
    tools::ToolRegistry& registry_;
    McpLimits limits_;
    common::Logger& logger_;

    mutable std::mutex mu_;  // session workers call EnsureAlive concurrently
    std::unordered_map<std::string, std::shared_ptr<ServerHandle>> handles_;
};

}  // namespace minimal::mcp
