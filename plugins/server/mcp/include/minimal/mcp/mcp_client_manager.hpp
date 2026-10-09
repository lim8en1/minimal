#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "minimal/mcp/channels_config.hpp"
#include "minimal/mcp/mcps_config.hpp"
#include "minimal/mcp/mcp_connection.hpp"
#include "minimal/tools/tool_registry.hpp"

namespace minimal::common {
class Logger;
}

namespace minimal::mcp {

// Everything the manager needs to also carry SIGNAL CHANNELS (channels.json
// entries): MCP stdio servers that push notifications into sessions instead
// of (only) offering tools to pull. `on_notification` is invoked with
// (channel_name, raw JSON-RPC notification) from the channel's read thread --
// it MUST NOT BLOCK, the same contract NotificationSink carries. It fires
// only for handles started as channels. `on_connected` is invoked once per
// successful (re)connect with (channel_name, instructions) -- the initialize
// result's instructions string, empty when the server sent none; the router
// stores it so a session's system-prompt section can carry the operator's
// framing. Both run under the handle's start_mu (never under mu_).
struct ChannelWiring {
    config::ChannelsConfig config;
    std::function<void(const std::string&, const std::string&)> on_notification;
    std::function<void(const std::string&, const std::string&)> on_connected;
};

// Owns the shared MCP server processes: ONE connection per enabled mcps.json
// entry, shared across every session (the low-footprint goal -- not one child
// process per session). Liveness is checked when a SESSION starts with a preset
// listing the server (or /agent switches to one), never at server boot; tool
// discovery happens once per server process and is cached on its handle.
// Re-discovery happens only when a server process restarts.
//
// Channel entries (ChannelWiring) share the exact same lifecycle -- one
// connection per enabled entry, the same reaper, the same restart cooldown,
// lazy first start on the first EnsureAlive (a session subscribing to the
// channel is what triggers it). The differences are additive: a channel
// handle carries its notification sink and its declared signals/mode, and
// StartHandle verifies the server actually declared the minimal/signal
// capability before marking it alive.
//
// This is a core mechanism, not a plugin: sessions' ToolExecutor, agent loop
// and subagent gating all consult it.
class McpClientManager {
public:
    // `config` is the parsed mcps.json; a handle is built for every entry
    // regardless of its `enabled` flag (SetEnabled toggles it live, without a
    // restart), but entries with recorded parse errors are skipped. `registry`
    // receives discovered tools under mcp__<server>__<tool>. Must outlive
    // every session that was handed tools from it (sessions borrow the
    // manager via the registry's tools).
    // `channels` defaults to none, so every existing call site keeps working;
    // when wiring carries entries, their names must not collide with MCP
    // server names -- a collision skips the CHANNEL (logged), keeping the
    // mcps.json entry authoritative.
    McpClientManager(config::McpsConfig config, tools::ToolRegistry& registry,
                     const McpLimits& limits, common::Logger& logger,
                     ChannelWiring channels = {});

    // Stops and joins the graveyard reaper. Any connection still live in
    // `handles_` at this point destructs normally, on this thread -- process
    // shutdown paying a bounded teardown cost once is not the bug; a session
    // worker paying it on every restart was.
    ~McpClientManager();

    struct Status {
        bool configured = false;  // present in mcps.json (regardless of enabled)
        bool enabled = false;     // runtime enabled flag (toggled via SetEnabled)
        bool alive = false;       // a live connection exists right now
        uint64_t generation = 0;  // bumped on every (re)connect; observable respawn
        std::string error;        // last failure, when !alive
        config::McpTransport transport = config::McpTransport::Stdio;
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

    // Live toggle, no restart. Enabling CONNECTS (bypassing the restart
    // cooldown -- an explicit user retry), since a session already past
    // activation would otherwise never bring the server up; false with
    // `error` set when it cannot, and a server that was disabled goes back
    // to disabled, so enabling again is the retry. On an already-enabled
    // server it is the reconnect. Disabling flips the flag, retires any live connection through the same
    // graveyard path StartHandle uses on restart (never destroys it inline
    // under a lock a concurrent CallTool needs), and retires the server's
    // tools from the registry immediately. Serialized against EnsureAlive via
    // the handle's start_mu, so a disable cannot race a StartHandle already
    // in flight. Returns false with `error` set if `server_name` was never in
    // mcps.json at all (a disabled-but-present entry is a valid target).
    bool SetEnabled(const std::string& server_name, bool enabled, std::string& error);

    Status StatusOf(const std::string& server_name) const;

    std::vector<std::string> ServerNames() const;
    // The mcps.json servers only -- ServerNames() minus channel handles.
    std::vector<std::string> ToolServerNames() const;
    bool IsConfigured(const std::string& server_name) const;
    // "local" (stdio: a process on this machine) or "remote" (http), for a
    // configured server; nullopt otherwise. The permission model's locality
    // dimension.
    std::optional<std::string> LocalityOf(const std::string& server_name) const;

    // Channel names that survived construction's reserved/collision rules AND
    // are currently runtime-enabled -- the channels this manager will
    // actually deliver notifications through right now. A caller validating
    // channel subscriptions (the signal router) must check against this, not
    // against channels.json directly, or it would validate a subscription to
    // a channel the manager never built a handle for, or one currently
    // disabled via SetEnabled.
    std::vector<std::string> EnabledChannelNames() const;

private:
    struct ServerHandle;
    class HandleInvoker;

    // Spawns/connects, initializes, discovers, and (re)registers the server's
    // tools into the registry. For a channel handle: attaches the notification
    // sink, verifies the minimal/signal capability (a channels.json entry
    // whose server does not declare it is a failed connect, not a silent
    // tool-only server), and reports instructions through the wiring.
    // Returns false with `error` set. Caller holds the handle's start_mu.
    bool StartHandle(const config::McpServerEntry& entry, ServerHandle& handle,
                     std::string& error);

    // EnsureAlive's body for a looked-up handle; caller holds its start_mu.
    // `honor_cooldown` false is SetEnabled's explicit attempt.
    bool EnsureAliveHeld(ServerHandle& handle, bool honor_cooldown, std::string& error);

    // Moves any live connection out of `handle` and into the graveyard for
    // off-thread teardown, marking the handle not-alive. Shared by
    // StartHandle (about to replace it with a fresh one) and SetEnabled's
    // disable path (not replacing it with anything).
    void RetireConnection(ServerHandle& handle);

    tools::ToolRegistry& registry_;
    McpLimits limits_;
    common::Logger& logger_;
    ChannelWiring channels_;

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
