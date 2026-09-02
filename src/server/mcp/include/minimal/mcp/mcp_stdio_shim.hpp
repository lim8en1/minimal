#pragma once

// The ONLY boundary between the harness and cpp-mcp's headers.
//
// cpp-mcp vendors its own older nlohmann/json (ABI tag v3_11_310) while the
// harness links vcpkg's (v3_12_0). Mixing the two headers in one translation
// unit produces two different types named nlohmann::ordered_json, and the
// static library's symbols will not bind. So every cpp-mcp type is kept inside
// mcp_stdio_shim.cpp -- which includes no harness headers -- and crosses back
// as plain strings, bools and ints. Text is the one lossless bridge.

#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace minimal::mcp {

class McpStdioShim {
public:
    // Does not spawn anything yet: `initialize` starts the subprocess.
    McpStdioShim(const std::string& command_line, const std::map<std::string, std::string>& env);
    ~McpStdioShim();
    McpStdioShim(const McpStdioShim&) = delete;
    McpStdioShim& operator=(const McpStdioShim&) = delete;

    struct ShimTool {
        std::string name;
        std::string description;
        std::string schema_json;  // serialized JSON, harness side re-parses
    };

    // The server-originated NOTIFICATION seam, for signal channels
    // (channels.json entries; see mcp_client_manager.hpp). The sink receives
    // the raw JSON-RPC notification -- one complete JSON object, serialized to
    // text so it crosses the shim's string boundary. The underlying callback
    // runs on cpp-mcp's read thread and the sink MUST NOT BLOCK: a slow sink
    // stalls response matching for every in-flight request on this connection.
    // Must be called before Initialize() (the read thread starts there and
    // reads the callback without a lock); a later call is silently ignored --
    // connection startup is single-threaded, so a race cannot hide here.
    void SetNotificationSink(std::function<void(const std::string&)> sink);

    // Spawns the child and runs the handshake. May block indefinitely if the
    // child dies during startup (cpp-mcp has no deadline here) -- callers own
    // the deadline. On false, *error (when non-null) carries the exception
    // message: "rejected the handshake" alone hid whether the child died,
    // crashed on startup, or answered a protocol error.
    bool Initialize(const std::string& client_name, const std::string& client_version,
                    std::string* error = nullptr);
    bool Ping();
    std::vector<ShimTool> ListTools();

    // The initialize result's `capabilities` object, serialized. The harness
    // parses it to detect the `minimal/signal` capability that marks a
    // connection as a signal channel. Empty object when the server sent none
    // or the connection is not initialized.
    std::string ServerCapabilitiesJson();

    // The initialize result's `instructions` string -- how a channel server
    // describes what its signals mean. Empty when the server sent none.
    std::string Instructions();

    // arguments_json is the serialized arguments object; on success
    // result_json holds the serialized tools/call result. A protocol-level
    // error (including a JSON-RPC error response) is reported through `error`
    // rather than an exception.
    bool CallTool(const std::string& name, const std::string& arguments_json,
                  std::string& result_json, std::string& error);

    // Best-effort SIGTERM of the child subprocess, for teardown while a worker
    // thread may be wedged inside a blocking call. Guarded by
    // waitpid(WNOHANG): only a live process that is still OUR child is
    // signalled, so a dead-and-reaped child never causes a reused pid to be
    // signalled. Children of the child are not touched (cpp-mcp spawns no
    // process group). Returns the signalled pid, or 0 when there is nothing
    // to signal; the caller feeds the result to ReapChild or
    // WatchChildDetached. Safe to call while another thread is inside
    // Initialize(): the client pointer is atomic and cpp-mcp's process_id_ is
    // atomic (vendored patch), so the pid is read the moment fork records it.
    int AbortChild();

    // Reaps a child AbortChild signalled, from the teardown thread: poll
    // waitpid(WNOHANG) for up to 5 s of SIGTERM grace, then SIGKILL and a
    // blocking reap. The blocking tail only runs on a child that ignored
    // SIGTERM. Call only once the worker thread has been joined -- reaping
    // under a live cpp-mcp client would race its own destructor's waitpid
    // (the loser seeing ECHILD is handled, but the child it expects to read
    // from is then gone for reasons it cannot see).
    static void ReapChild(int pid);

    // The detached escalation for a worker that could NOT be joined: the
    // process may exit before a wedged worker unwinds, so nobody can block
    // on the child. A short detached watcher reaps with the same SIGTERM
    // grace and SIGKILL escalation as ReapChild.
    static void WatchChildDetached(int pid);

private:
    std::string command_line_;
    std::map<std::string, std::string> env_;
    // Parked pre-initialize notification sink (SetNotificationSink), attached
    // to the client in Initialize() before the child spawns.
    std::function<void(const std::string&)> notification_sink_;
    // Atomic: AbortChild reads it from the destroying thread while the worker
    // may still be inside Initialize() assigning it -- a plain pointer there
    // is a data race (TSan, the merge's own review). Store happens after the
    // object's constructor, load before any member call, so an observed
    // pointer is to a constructed client.
    std::atomic<void*> client_{nullptr};  // opaque: mcp::stdio_client, hidden from this header
};

}  // namespace minimal::mcp
