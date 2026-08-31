#pragma once

// The ONLY boundary between the harness and cpp-mcp's headers.
//
// cpp-mcp vendors its own older nlohmann/json (ABI tag v3_11_310) while the
// harness links vcpkg's (v3_12_0). Mixing the two headers in one translation
// unit produces two different types named nlohmann::ordered_json, and the
// static library's symbols will not bind. So every cpp-mcp type is kept inside
// mcp_stdio_shim.cpp -- which includes no harness headers -- and crosses back
// as plain strings, bools and ints. Text is the one lossless bridge.

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

    // Spawns the child and runs the handshake. May block indefinitely if the
    // child dies during startup (cpp-mcp has no deadline here) -- callers own
    // the deadline. On false, *error (when non-null) carries the exception
    // message: "rejected the handshake" alone hid whether the child died,
    // crashed on startup, or answered a protocol error.
    bool Initialize(const std::string& client_name, const std::string& client_version,
                    std::string* error = nullptr);
    bool Ping();
    std::vector<ShimTool> ListTools();

    // arguments_json is the serialized arguments object; on success
    // result_json holds the serialized tools/call result. A protocol-level
    // error (including a JSON-RPC error response) is reported through `error`
    // rather than an exception.
    bool CallTool(const std::string& name, const std::string& arguments_json,
                  std::string& result_json, std::string& error);

    // The spawned child's pid, -1 before Initialize. Reads cpp-mcp's atomic
    // field, so it is safe to call from the teardown thread while a worker
    // thread is still inside a blocking call (that is the whole point).
    int ChildPid() const;

    // Best-effort kill of the child subprocess, for teardown while a worker
    // thread may be wedged inside a blocking call: SIGTERM, with a detached
    // watcher escalating to SIGKILL after 5 s. Guarded by waitpid(WNOHANG):
    // only a live process that is still OUR child is signalled, so a dead-
    // and-reaped child never causes a reused pid to be signalled. Children of
    // the child are not touched (cpp-mcp spawns no process group).
    void AbortChild();

private:
    std::string command_line_;
    std::map<std::string, std::string> env_;
    void* client_ = nullptr;  // opaque: mcp::stdio_client, hidden from this header
};

}  // namespace minimal::mcp
