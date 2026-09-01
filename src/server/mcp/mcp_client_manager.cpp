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
    mutable std::mutex mu;  // guards connection/alive/generation/verified swaps
    // Serializes ping + (re)start for THIS server. All network I/O for the
    // handle happens under it -- never under mu_, so other servers' checks
    // and the cheap Status/ServerNames lookups never wait behind a wedged or
    // slow server.
    std::mutex start_mu;
    std::shared_ptr<McpConnection> connection;
    bool alive = false;
    uint64_t generation = 0;
    std::string error;
    std::chrono::steady_clock::time_point verified{};  // epoch = never
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
            return {false, "MCP server is no longer configured", nlohmann::json()};
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
                return {false, "MCP server '" + handle->entry.name + "' is not connected: " + error,
                        nlohmann::json()};
            }
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
        McpCallOutcome outcome = connection->CallTool(tool_name, arguments, cancel);
        if (!outcome.ok) {
            // !ok is the transport/protocol layer failing (dead process,
            // timeout, malformed response) -- never a tool-level error, which
            // comes back ok=true with an isError result. Marking the handle
            // not-alive here is what makes "the next tool call respawns it"
            // true rather than aspirational: this exact call does not retry
            // (a stuck cancel token or a hung caller must not be doubled),
            // but the NEXT one takes the need_restart branch above.
            std::lock_guard<std::mutex> lock(handle->mu);
            handle->alive = false;
        }
        return outcome;
    }

private:
    std::weak_ptr<ServerHandle> handle_;
    McpClientManager* manager_;
};

McpClientManager::McpClientManager(config::McpsConfig config, tools::ToolRegistry& registry,
                                   const McpLimits& limits, common::Logger& logger)
    : registry_(registry), limits_(limits), logger_(logger) {
    // The config is consumed here: handles carry their own entry copies, and
    // a second copy of the whole mcps.json is dead weight for the process
    // lifetime.
    for (const auto& entry : config.entries) {
        if (!entry.enabled) continue;  // deliberate opt-out, silent
        auto handle = std::make_shared<ServerHandle>();
        handle->entry = entry;
        handle->invoker = std::make_shared<HandleInvoker>(handle, this);
        handles_.emplace(entry.name, std::move(handle));
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
    auto lookup = [](std::string_view name) -> std::optional<std::string> {
        const char* value = std::getenv(std::string(name).c_str());
        if (value == nullptr) return std::nullopt;
        return std::string(value);
    };
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

bool McpClientManager::StartHandle(const config::McpServerEntry& entry, ServerHandle& handle,
                                   std::string& error) {
    // Caller holds handle.start_mu. Close any previous connection first: a
    // dead child must not linger, and the swap must be atomic from the
    // invoker's point of view (a concurrent CallTool holds the old
    // connection alive through its shared_ptr copy). The old connection is
    // MOVED OUT under handle.mu, not destroyed under it -- ~McpConnection's
    // teardown blocks (worker join, then a SIGTERM-grace-then-SIGKILL reap),
    // and handle.mu is the same lock CallTool takes just to read the live
    // connection pointer. Handing it to the graveyard lets the reaper thread
    // pay that cost instead of every concurrent tool call stalling behind it.
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

    const auto resolved = ResolveEndpoint(entry, error, logger_);
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
        handle.verified = std::chrono::steady_clock::now();
    }
    logger_.info("MCP server '{}': connected, {} tools registered", entry.name, registered);
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
            error = "MCP server '" + server_name + "' is not configured (mcps.json) or is disabled";
            return false;
        }
        handle = it->second;
    }

    // One ping/restarter at a time per server. A caller that waited behind a
    // successful one falls into the fresh-verdict path below and skips its
    // own ping entirely.
    std::lock_guard<std::mutex> start_lock(handle->start_mu);

    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(handle->mu);
        if (handle->alive && handle->connection && now - handle->verified < kVerifiedTtl) {
            // The liveness check the architecture asks for at session start,
            // answered from the recent verdict rather than the wire.
            return true;
        }
    }

    if (handle->alive && handle->connection) {
        McpConnection::Failure ping_error;
        if (handle->connection->Ping(nullptr, ping_error)) {
            std::lock_guard<std::mutex> lock(handle->mu);
            handle->verified = std::chrono::steady_clock::now();
            return true;
        }
        logger_.warn("MCP server '{}' liveness check failed ({}); restarting", server_name,
                     ping_error.message);
    }

    if (StartHandle(handle->entry, *handle, error)) return true;
    {
        std::lock_guard<std::mutex> lock(handle->mu);
        handle->alive = false;
        handle->error = error;
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
