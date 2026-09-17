#include "minimal/mcp/mcp_client_manager.hpp"

#include <optional>
#include <utility>

#include "minimal/common/config_paths.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/mcp/env_interpolation.hpp"
#include "minimal/mcp/naming.hpp"
#include "minimal/mcp/signal_protocol.hpp"

namespace minimal::mcp {

// Per-server shared state. The invoker handed to McpTool holds a weak_ptr to
// the handle and reads `connection` under the manager's mutex, so a respawn
// (which swaps the connection) is atomic from a tool call's point of view --
// and an in-flight call keeps the OLD connection alive through its own
// shared_ptr copy.
struct McpClientManager::ServerHandle {
    config::McpServerEntry entry;
    // Set when the handle came from channels.json: the entry carries the
    // channel's connection fields, and `channel` its signal contract. The
    // notification sink is built per-connection in StartHandle (it must be
    // handed over before the handshake), not stored here.
    bool is_channel = false;
    config::ChannelEntry channel;
    std::shared_ptr<HandleInvoker> invoker;
    mutable std::mutex mu;  // guards connection/alive/generation/verified/enabled swaps
    // Serializes ping + (re)start for THIS server. All network I/O for the
    // handle happens under it -- never under mu_, so other servers' checks
    // and the cheap Status/ServerNames lookups never wait behind a wedged or
    // slow server. SetEnabled also takes it, so a disable cannot race a
    // StartHandle already in flight for the same handle.
    std::mutex start_mu;
    std::shared_ptr<McpConnection> connection;
    bool alive = false;
    // Runtime toggle (SetEnabled), seeded from mcps.json's `enabled` at
    // construction. A handle exists for every configured entry regardless of
    // this flag -- only whether EnsureAlive is willing to connect it depends
    // on it.
    bool enabled = true;
    uint64_t generation = 0;
    std::string error;
    std::chrono::steady_clock::time_point verified{};      // epoch = never
    std::chrono::steady_clock::time_point last_failure{};  // epoch = never failed
};

// The McpInvoker implementation McpTool carries. Weak to the handle: if the
// manager itself is gone (teardown while a session worker still runs a tool),
// the call degrades to a clean error instead of crashing.
class McpClientManager::HandleInvoker final : public McpInvoker {
public:
    // `manager` is a raw back-pointer, safe because HandleInvoker is owned
    // (transitively, through ServerHandle) by manager->handles_ -- it cannot
    // outlive the manager that constructed it.
    HandleInvoker(std::weak_ptr<ServerHandle> handle, McpClientManager* manager)
        : handle_(std::move(handle)), manager_(manager) {}

    McpCallOutcome CallTool(std::string_view tool_name, const nlohmann::json& arguments,
                            const tools::CancelTokenPtr& cancel) override {
        std::shared_ptr<ServerHandle> handle = handle_.lock();
        if (handle == nullptr) {
            // Designated initializers: the positional form silently bound the
            // trailing json() onto caller_cancelled (bool), leaving `result`
            // unmentioned -- the -Wmissing-field-initializers that is exactly
            // the point of naming every field on a multi-flag outcome.
            return {.ok = false, .error = "MCP server is no longer configured"};
        }
        bool need_restart;
        {
            std::lock_guard<std::mutex> lock(handle->mu);
            need_restart = !handle->alive || !handle->connection;
        }
        // A server that died mid-session used to error "not connected" until
        // the NEXT session start or /agent switch re-checked it -- the
        // respawn the header comment already claimed happened here. It does
        // now: EnsureAlive is the same per-server-gated (start_mu) path a
        // session's own liveness check uses, so a storm of calls against a
        // down server is still serialized to one restart attempt, and (once
        // cooldown lands) rate-limited across restarts too.
        if (need_restart) {
            std::string error;
            if (!manager_->EnsureAlive(handle->entry.name, error)) {
                return {.ok = false,
                        .error = "MCP server '" + handle->entry.name + "' is not connected: " + error};
            }
        }
        std::shared_ptr<McpConnection> connection;
        {
            std::lock_guard<std::mutex> lock(handle->mu);
            connection = handle->connection;
            if (!handle->alive || !connection) {
                return {.ok = false,
                        .error = "MCP server '" + handle->entry.name + "' is not connected"};
            }
        }
        McpCallOutcome outcome = connection->CallTool(tool_name, arguments, cancel);
        if (!outcome.ok) {
            // !ok is the transport/protocol layer failing (dead process,
            // timeout, malformed response) -- never a tool-level error, which
            // comes back ok=true with an isError result. Marking the handle
            // not-alive here is what makes "the next tool call respawns it"
            // true rather than aspirational: this exact call does not retry
            // (a stuck cancel token or a hung caller must not be doubled),
            // but the NEXT one takes the need_restart branch above.
            //
            // EXCEPT caller cancellation: WaitSlot reports the CALLER's token
            // firing (a turn abort, a tab close) as the typed
            // outcome.caller_cancelled. The server did nothing wrong -- the
            // MCP job keeps running server-side, and its state is the user's
            // to keep. Killing the connection here would respawn (and drop
            // every server-side session) on a routine turn abort, so a
            // cancelled call leaves `alive` untouched. A timeout still kills:
            // the worker is single-threaded, and a call that wedges its
            // transport must otherwise wedge every later call with it.
            const bool caller_cancelled = outcome.caller_cancelled;
            if (!caller_cancelled) {
                std::lock_guard<std::mutex> lock(handle->mu);
                handle->alive = false;
            }
        }
        return outcome;
    }

private:
    std::weak_ptr<ServerHandle> handle_;
    McpClientManager* manager_;
};

McpClientManager::McpClientManager(config::McpsConfig config, tools::ToolRegistry& registry,
                                   const McpLimits& limits, common::Logger& logger,
                                   ChannelWiring channels)
    : registry_(registry), limits_(limits), logger_(logger), channels_(std::move(channels)) {
    // The config is consumed here: handles carry their own entry copies, and
    // a second copy of the whole mcps.json is dead weight for the process
    // lifetime.
    for (const auto& entry : config.entries) {
        auto handle = std::make_shared<ServerHandle>();
        handle->entry = entry;
        handle->enabled = entry.enabled;
        handle->invoker = std::make_shared<HandleInvoker>(handle, this);
        handles_.emplace(entry.name, std::move(handle));
    }
    // Channel handles join the same map and lifecycle. A name that collides
    // with an MCP server keeps the mcps.json side: the channel is skipped
    // loudly here, and the startup sanity check reports the collision from
    // the config side -- two lines naming the same problem once each.
    for (const auto& entry : channels_.config.entries) {
        // The internal source is not addressable by config: a channel with
        // this name would be unreachable from subscriptions and would collide
        // with cross-session routing. The sanity check names it too; this is
        // the enforcement.
        if (entry.name == config::kReservedInternalChannelName) {
            logger_.error("signal channel '{}': the name is reserved for internal "
                          "cross-session signals; NOT configured",
                          entry.name);
            continue;
        }
        auto handle = std::make_shared<ServerHandle>();
        handle->is_channel = true;
        handle->channel = entry;
        handle->enabled = entry.enabled;
        // The channel's CONNECTION fields ride the same McpServerEntry view
        // the stdio path already understands (v1 is stdio-only, so the
        // conversion is command/args/env and nothing else).
        handle->entry.name = entry.name;
        handle->entry.transport = config::McpTransport::Stdio;
        handle->entry.command = entry.command;
        handle->entry.args = entry.args;
        handle->entry.env = entry.env;
        handle->invoker = std::make_shared<HandleInvoker>(handle, this);
        if (!handles_.emplace(entry.name, std::move(handle)).second) {
            logger_.error("signal channel '{}': name collides with an MCP server in mcps.json; "
                          "the channel is NOT configured",
                          entry.name);
        }
    }
    reaper_ = std::thread([this] { ReaperMain(); });
}

McpClientManager::~McpClientManager() {
    stop_reaper_.store(true, std::memory_order_release);
    graveyard_cv_.notify_one();
    if (reaper_.joinable()) reaper_.join();
}

void McpClientManager::ReaperMain() {
    while (!stop_reaper_.load(std::memory_order_acquire)) {
        std::vector<std::shared_ptr<McpConnection>> doomed;
        {
            std::unique_lock<std::mutex> lock(graveyard_mu_);
            // The periodic timeout is a belt-and-suspenders wakeup in case a
            // notify races the wait (a push between the stop check above and
            // the wait below); nothing here depends on it firing promptly.
            graveyard_cv_.wait_for(lock, std::chrono::milliseconds(500), [this] {
                return stop_reaper_.load(std::memory_order_acquire) || !graveyard_.empty();
            });
            doomed.swap(graveyard_);
        }
        // Destructors run here, off the lock, off any session worker: this is
        // the whole point -- ~McpConnection's blocking teardown (worker join,
        // then SIGTERM-grace-then-SIGKILL reap) costs whatever it costs, but
        // pays it on the reaper thread, not a caller's.
        doomed.clear();
    }
}

namespace {

// How long a successful liveness check counts as proof of life. A subagent
// spawn right after the parent verified the same server, a child session
// starting while another caller's restart is still in flight, and other
// back-to-back checks reuse the verdict instead of paying another ping round
// trip (a full TCP+TLS handshake on the http transport). Deliberately SHORT:
// a server that died 3 s ago must still be caught by the next session's
// check -- the TTL is a dedup for near-simultaneous callers, not a general
// liveness cache. (A server that dies inside the window is still surfaced
// immediately by the next tool call, which respawns the handle.)
constexpr auto kVerifiedTtl = std::chrono::seconds(2);

std::optional<config::McpServerEntry> ResolveEndpoint(const config::McpServerEntry& entry,
                                                      std::string& error,
                                                      common::Logger& logger) {
    // ${ENV_VAR} interpolation happens at connect time, never logged, per
    // mcps_config.hpp. An unresolvable marker is a connect failure naming the
    // ENTRY -- never the value.
    config::McpServerEntry resolved = entry;
    auto lookup = [](std::string_view name) { return common::LookupEnv(name); };
    // A bare `$NAME` is not interpolated (only `${NAME}` is) -- documented,
    // intentional passthrough. It is also indistinguishable at a glance from a
    // forgotten pair of braces, so a plausible-looking one gets a warning
    // (never a value, per the no-secrets-logged rule above).
    auto warn_bare = [&logger, &entry](std::string_view name) {
        logger.warn("MCP server '{}': a bare '${}' is not interpolated (only "
                    "the braced form is); passed through literally",
                    entry.name, name);
    };
    for (auto& [key, value] : resolved.env) {
        std::string marker;
        if (auto interpolated = InterpolateEnv(value, lookup, &marker, warn_bare)) {
            value = *std::move(interpolated);
        } else {
            error = "MCP server '" + entry.name + "': env '" + key +
                    "' references an unset variable '" + marker + "'";
            return std::nullopt;
        }
    }
    for (auto& [key, value] : resolved.headers) {
        std::string marker;
        if (auto interpolated = InterpolateEnv(value, lookup, &marker, warn_bare)) {
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

void McpClientManager::RetireConnection(ServerHandle& handle) {
    // Close any previous connection first: a dead child must not linger, and
    // the swap must be atomic from the invoker's point of view (a concurrent
    // CallTool holds the old connection alive through its shared_ptr copy).
    // The old connection is MOVED OUT under handle.mu, not destroyed under
    // it -- ~McpConnection's teardown blocks (worker join, then a
    // SIGTERM-grace-then-SIGKILL reap), and handle.mu is the same lock
    // CallTool takes just to read the live connection pointer. Handing it to
    // the graveyard lets the reaper thread pay that cost instead of every
    // concurrent tool call stalling behind it.
    std::shared_ptr<McpConnection> old_connection;
    {
        std::lock_guard<std::mutex> lock(handle.mu);
        old_connection = std::move(handle.connection);
        handle.alive = false;
    }
    if (old_connection) {
        std::lock_guard<std::mutex> lock(graveyard_mu_);
        graveyard_.push_back(std::move(old_connection));
        graveyard_cv_.notify_one();
    }
}

bool McpClientManager::StartHandle(const config::McpServerEntry& entry, ServerHandle& handle,
                                   std::string& error) {
    // Caller holds handle.start_mu.
    RetireConnection(handle);

    // A connection that opened successfully but fails a LATER check (missing
    // capability, tool-discovery error, an unnamespaceable server name) must
    // not have its ~McpConnection run inline here: teardown blocks (worker
    // join, then a SIGTERM-grace-then-SIGKILL reap), same as RetireConnection
    // above, and StartHandle runs on session/subagent worker threads (via
    // EnsureAlive) the graveyard exists specifically to keep off that cost.
    auto retire_failed_connection = [this](std::unique_ptr<McpConnection> failed) {
        if (!failed) return;
        std::lock_guard<std::mutex> lock(graveyard_mu_);
        graveyard_.push_back(std::shared_ptr<McpConnection>(std::move(failed)));
        graveyard_cv_.notify_one();
    };

    const auto resolved = ResolveEndpoint(entry, error, logger_);
    if (!resolved) return false;

    // The notification sink must exist BEFORE the handshake: the read thread
    // that invokes it starts during initialize. Built fresh per connection
    // (each connection carries its own copy); it captures only the channel
    // name and the wiring callbacks, never the handle -- a graveyarded old
    // connection whose read thread fires one last frame must not touch a
    // handle that may already be restarted or destroyed.
    NotificationSink sink;
    if (handle.is_channel) {
        auto on_notification = channels_.on_notification;
        const std::string channel_name = entry.name;
        if (on_notification) {
            sink = [on_notification, channel_name](const std::string& raw) {
                on_notification(channel_name, raw);
            };
        }
    }

    McpConnection::Failure failure;
    auto connection = McpConnection::Open(ToEndpoint(*resolved), limits_, "minimal-server",
                                          "1.0.0b", failure, sink);
    if (!connection) {
        error = "MCP server '" + entry.name + "' failed to connect: " + failure.message;
        return false;
    }

    // A channels.json entry whose server does not declare the capability is a
    // failed connect, not a quietly tool-only server: the operator configured
    // a channel, and silence about a missing capability would look exactly
    // like "connected but no signals ever arrive".
    if (handle.is_channel) {
        const nlohmann::json capabilities =
            nlohmann::json::parse(connection->ServerCapabilitiesJson(), nullptr,
                                  /*allow_exceptions=*/false);
        const bool declares = capabilities.is_object() &&
                              capabilities.contains("experimental") &&
                              capabilities.at("experimental").is_object() &&
                              capabilities.at("experimental").contains(mcp::kSignalCapability);
        if (!declares) {
            error = "signal channel '" + entry.name +
                    "' does not declare the experimental '" + mcp::kSignalCapability +
                    "' capability; not usable as a channel";
            retire_failed_connection(std::move(connection));
            return false;
        }
    }

    McpConnection::Failure tools_error;
    const auto listings = connection->ListTools(nullptr, tools_error);
    if (!listings) {
        error = "MCP server '" + entry.name + "' tool discovery failed: " + tools_error.message;
        retire_failed_connection(std::move(connection));
        return false;
    }

    // Validate the namespace BEFORE registering: a server (or config) name
    // that would make mcp__<server>__<tool> ambiguous skips registration with
    // a named reason rather than corrupting the wire namespace.
    if (!IsValidMcpServerName(entry.name)) {
        error = "MCP server name '" + entry.name +
                "' cannot be namespaced (contains '__', ends with '_', or contains whitespace "
                "or control characters)";
        retire_failed_connection(std::move(connection));
        return false;
    }

    auto invoker = handle.invoker;
    std::vector<std::unique_ptr<tools::Tool>> tools;
    std::size_t registered = 0;
    for (const auto& listing : *listings) {
        if (!IsValidMcpToolName(listing.name)) {
            logger_.warn("MCP server '{}' tool '{}' skipped: name contains '__', ends with '_', "
                         "or contains whitespace or control characters",
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

    // Captured BEFORE the connection is moved into the handle: the
    // instructions live on it, and the move nulls the local unique_ptr.
    // Reported after the handle is marked alive, under start_mu, per the
    // wiring contract -- ordering-independent of the alive flag either way
    // (the sink is already live).
    const std::string channel_instructions =
        handle.is_channel ? connection->Instructions() : std::string();

    {
        std::lock_guard<std::mutex> lock(handle.mu);
        handle.connection = std::move(connection);
        handle.alive = true;
        ++handle.generation;
        handle.error.clear();
        handle.verified = std::chrono::steady_clock::now();
    }
    logger_.info("MCP server '{}': connected, {} tools registered", entry.name, registered);

    if (handle.is_channel && channels_.on_connected) {
        channels_.on_connected(entry.name, channel_instructions);
    }
    return true;
}

bool McpClientManager::EnsureAlive(const std::string& server_name, std::string& error) {
    // mu_ is held ONLY for the map lookup -- never across network I/O. All
    // blocking work (ping, connect, discovery) happens under the handle's
    // own start_mu, so a slow or wedged server delays only its own callers;
    // other sessions' checks and Status/ServerNames proceed.
    std::shared_ptr<ServerHandle> handle;
    {
        std::lock_guard<std::mutex> lock(mu_);
        const auto it = handles_.find(server_name);
        if (it == handles_.end()) {
            error = "MCP server '" + server_name + "' is not configured (mcps.json)";
            return false;
        }
        handle = it->second;
    }

    // One ping/restarter at a time per server. A caller that waited behind a
    // successful one falls into the fresh-verdict path below and skips its
    // own ping entirely. Also serializes against SetEnabled: a disable that
    // grabbed start_mu first is fully applied (flag flipped, connection
    // retired) before this check runs, and a disable that arrives after this
    // check waits behind whatever StartHandle this call is about to do.
    std::lock_guard<std::mutex> start_lock(handle->start_mu);

    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(handle->mu);
        if (!handle->enabled) {
            error = "MCP server '" + server_name + "' is disabled";
            return false;
        }
        if (handle->alive && handle->connection && now - handle->verified < kVerifiedTtl) {
            // The liveness check the architecture asks for at session start,
            // answered from the recent verdict rather than the wire.
            return true;
        }
    }

    // Snapshot alive+connection together under handle->mu before pinging:
    // reading them unguarded here raced HandleInvoker::CallTool's failure
    // path, which flips handle->alive under handle->mu alone (UB under the
    // memory model, not merely a stale read).
    std::shared_ptr<McpConnection> connection;
    {
        std::lock_guard<std::mutex> lock(handle->mu);
        if (handle->alive) connection = handle->connection;
    }
    if (connection) {
        McpConnection::Failure ping_error;
        if (connection->Ping(nullptr, ping_error)) {
            std::lock_guard<std::mutex> lock(handle->mu);
            handle->verified = std::chrono::steady_clock::now();
            return true;
        }
        logger_.warn("MCP server '{}' liveness check failed ({}); restarting", server_name,
                     ping_error.message);
    }

    // A restart ATTEMPT is what's rate-limited, not a liveness check itself:
    // a healthy, reused connection never reaches this line at all (the TTL
    // and ping branches above return first). Without this, a subagent spawn
    // loop against a down server pays a full spawn+initialize+discovery
    // attempt per spawn, at spawn rate -- exactly the storm CallTool's new
    // respawn-on-call path (W9.2a) would otherwise invite.
    {
        std::lock_guard<std::mutex> lock(handle->mu);
        const auto since_failure = std::chrono::steady_clock::now() - handle->last_failure;
        if (handle->last_failure != std::chrono::steady_clock::time_point{} &&
            since_failure < limits_.restart_cooldown) {
            error = "MCP server '" + server_name + "' is in restart cooldown after a recent failure";
            return false;
        }
    }

    if (StartHandle(handle->entry, *handle, error)) return true;
    {
        std::lock_guard<std::mutex> lock(handle->mu);
        handle->alive = false;
        handle->error = error;
        handle->last_failure = std::chrono::steady_clock::now();
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
        status.enabled = handle.enabled;
        status.alive = handle.alive;
        status.generation = handle.generation;
        status.error = handle.error;
        status.transport = handle.entry.transport;
        return status;
    }
}

bool McpClientManager::SetEnabled(const std::string& server_name, bool enabled, std::string& error) {
    std::shared_ptr<ServerHandle> handle;
    {
        std::lock_guard<std::mutex> lock(mu_);
        const auto it = handles_.find(server_name);
        if (it == handles_.end()) {
            error = "MCP server '" + server_name + "' is not configured (mcps.json)";
            return false;
        }
        handle = it->second;
    }
    // Serializes against EnsureAlive/StartHandle for this handle -- see the
    // comment on EnsureAlive's start_lock.
    std::lock_guard<std::mutex> start_lock(handle->start_mu);
    if (!enabled) {
        RetireConnection(*handle);
        registry_.ReplaceMcpTools(server_name, {});
    }
    {
        std::lock_guard<std::mutex> lock(handle->mu);
        handle->enabled = enabled;
    }
    return true;
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

std::vector<std::string> McpClientManager::EnabledChannelNames() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<std::string> names;
    for (const auto& [name, handle] : handles_) {
        if (!handle->is_channel) continue;
        std::lock_guard<std::mutex> handle_lock(handle->mu);
        if (handle->enabled) names.push_back(name);
    }
    return names;
}

}  // namespace minimal::mcp
