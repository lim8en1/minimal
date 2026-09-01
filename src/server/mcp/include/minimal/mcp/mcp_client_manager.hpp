#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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

    // Stops and joins the graveyard reaper. Any connection still live in
    // `handles_` at this point destructs normally, on this thread -- process
    // shutdown paying a bounded teardown cost once is not the bug; a session
    // worker paying it on every restart was.
    ~McpClientManager();

    struct Status {
        bool configured = false;  // present and enabled in mcps.json
        bool alive = false;       // a live connection exists right now
        uint64_t generation = 0;  // bumped on every (re)connect; observable respawn
        std::string error;        // last failure, when !alive
    };

    // Liveness check at session start / /agent switch, and (via HandleInvoker)
    // on any tool call against a handle not currently marked alive. Ensures
    // the shared connection exists and pings it; on failure the server is
    // marked dead and the error is surfaced (value, not exception) so the
    // caller can proceed without it. A dead server is RESPAWNED here --
    // re-discovery is exactly what a server process restart means -- unless a
    // restart attempt failed within the last restart_cooldown_ms, in which
    // case this returns false immediately naming the cooldown.
    //
    // Blocking shape: the manager-wide lock covers only the handle lookup;
    // the ping/restart I/O runs under a per-server mutex, so slow servers
    // stall only their own callers. A verdict younger than kVerifiedTtl (see
    // the .cpp) is reused without a wire round trip.
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

    tools::ToolRegistry& registry_;
    McpLimits limits_;
    common::Logger& logger_;

    mutable std::mutex mu_;  // session workers call EnsureAlive concurrently
    std::unordered_map<std::string, std::shared_ptr<ServerHandle>> handles_;

    // A restarted server's OLD connection lands here instead of being
    // destroyed inline by StartHandle: ~McpConnection's teardown blocks for up
    // to ~250 ms (worker join) plus up to another 5 s (SIGTERM grace before
    // SIGKILL) -- unacceptable held under `ServerHandle::mu`, which
    // HandleInvoker::CallTool also takes just to read the live connection
    // pointer. The reaper thread drains this vector and lets the teardown run
    // off any session worker. Modeled on SessionRegistry's reaper
    // (session_registry.cpp).
    std::mutex graveyard_mu_;
    std::condition_variable graveyard_cv_;
    std::vector<std::shared_ptr<McpConnection>> graveyard_;
    std::atomic<bool> stop_reaper_{false};
    std::thread reaper_;

    void ReaperMain();
};

}  // namespace minimal::mcp
