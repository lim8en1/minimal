#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "minimal/mcp/mcp_tool.hpp"

namespace minimal::mcp {

enum class McpTransport { Stdio, Http };

// `command_line` is the stdio command JOINED WITH SPACES -- cpp-mcp splits it
// on whitespace, so an argument containing a space cannot be passed (recorded
// limitation, ARCHITECTURE.md known gaps). `env`/`headers` arrive
// pre-interpolated: the manager resolves ${ENV_VAR} at connect time and never
// logs the values.
struct McpEndpoint {
    McpTransport transport = McpTransport::Stdio;
    std::string command_line;
    std::string url;
    std::unordered_map<std::string, std::string> env;
    std::unordered_map<std::string, std::string> headers;
};

struct McpLimits {
    std::chrono::milliseconds connect{10000};
    std::chrono::milliseconds request{60000};
    // tools/list pagination cap (http transport). 0 is never legal: the
    // settings loader rejects it; the default matches settings_config.hpp.
    std::int64_t max_list_pages{100};
    // McpClientManager-only (not read by McpConnection/the transports below):
    // the floor between restart ATTEMPTS for one server after a failure.
    // Carried here rather than as its own constructor parameter because
    // McpLimits is already the one settings-derived bundle the manager holds.
    std::chrono::milliseconds restart_cooldown{30000};
};

// The server-originated-notification seam (signal channels). The sink
// receives the raw JSON-RPC notification line from the connection's read
// thread and MUST NOT BLOCK -- a slow sink stalls response matching for every
// in-flight request on the connection. Nullopt/empty means no sink: the
// connection behaves exactly as before (notifications are logged and
// dropped).
using NotificationSink = std::function<void(const std::string&)>;

// One live MCP server connection -- either a spawned stdio subprocess (cpp-mcp)
// or a Streamable-HTTP endpoint (hand-rolled over net::HttpClient + SseParser;
// cpp-mcp's bundled sse_client speaks the legacy SSE protocol and cannot talk
// to a modern streamable-http server).
//
// ALL protocol I/O runs on one dedicated thread. Two reasons, both structural:
// cpp-mcp's client blocks internally (up to a fixed 60 s) and offers no
// deadline parameter, so a caller-visible timeout or a CancelTurn could not be
// honoured without keeping the blocking call off the caller's thread; and one
// thread serialises calls to the shared server, which sidesteps cpp-mcp's
// unverified concurrency. Callers wait on their cancellation token's
// condition variable (tools::CancelToken): completion and cancellation both
// notify it, so a waiter wakes on exactly the events that can change its
// outcome -- no polling. The underlying request may still complete in the
// background (accepted asymmetry -- the same class as bash's killpg window). A
// worker wedged in a blocking call is never joined: it holds its state by
// shared_ptr and exits when the call finally returns (the stdio child is
// killed on teardown, which unblocks it), so destruction cannot hang on a
// dead server.
class McpConnection : public McpInvoker {
public:
    struct ToolListing {
        std::string name;        // bare name, as the server knows it
        std::string description;
        nlohmann::json input_schema;
    };

    struct Failure {
        std::string message;
    };

    // The per-endpoint I/O implementation, behind the worker thread. Declared
    // here so the anonymous-namespace transports below can inherit it; defined
    // entirely in the .cpp.
    struct Transport;

    // Spawns/connects and completes the MCP initialize handshake. nullptr with
    // `error` set on failure -- always a bounded wait, never a hang, even if
    // the subprocess dies during startup (cpp-mcp's initialize can block
    // indefinitely there; the deadline lives here). `sink` (stdio only)
    // receives server-originated notifications; it must be handed over HERE,
    // before the handshake starts -- the read thread that invokes it is
    // spawned during initialize and reads the callback without a lock.
    static std::unique_ptr<McpConnection> Open(const McpEndpoint& endpoint,
                                               const McpLimits& limits,
                                               const std::string& client_name,
                                               const std::string& client_version,
                                               Failure& error,
                                               const NotificationSink& sink = nullptr);

    ~McpConnection() override;
    McpConnection(const McpConnection&) = delete;
    McpConnection& operator=(const McpConnection&) = delete;

    // The initialize result, as far as a channel cares: the server's
    // `capabilities` object (serialized JSON; the manager greps it for the
    // minimal/signal capability) and its `instructions` string. Read AFTER
    // Open() returns -- initialize wrote them on the worker and the Open
    // slot-wait gives the happens-before, so the reads need no lock. Not
    // transport I/O: plain member reads, never a round trip.
    std::string ServerCapabilitiesJson() const;
    std::string Instructions() const;

    // Liveness probe for session start and /agent switches. Value, not
    // exception: a dead server is an expected state, not a crash.
    bool Ping(const tools::CancelTokenPtr& cancel, Failure& error);

    // tools/list. nullopt with `error` set on failure.
    std::optional<std::vector<ToolListing>> ListTools(const tools::CancelTokenPtr& cancel,
                                                      Failure& error);

    // McpInvoker -- the path McpTool::execute takes. Always returns a value;
    // timeouts, dead servers, protocol errors and cancellation all become
    // outcome.error. Never throws, never outlives its deadline by design (the
    // background request itself may). A null token means no cancellation
    // path: deadline-only, which is also what tests that never cancel want.
    McpCallOutcome CallTool(std::string_view tool_name, const nlohmann::json& arguments,
                            const tools::CancelTokenPtr& cancel) override;

private:
    struct WorkerState;

    McpConnection(std::shared_ptr<WorkerState> worker, std::shared_ptr<Transport> transport,
                  std::thread loop);
    // Idempotent: signals the worker to stop and detaches the thread. A worker
    // wedged in a blocking call is never joined -- it holds its state by
    // shared_ptr and exits when the call finally returns.
    void Teardown();

    std::shared_ptr<WorkerState> worker_;
    std::shared_ptr<Transport> transport_;  // shared: every queued job captures a copy
    std::thread loop_;
    std::chrono::milliseconds request_timeout_{60000};
};

}  // namespace minimal::mcp
