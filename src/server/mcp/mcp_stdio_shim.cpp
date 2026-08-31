// Compiled as an ISLAND: include cpp-mcp's headers (which pull their own
// bundled nlohmann/json) and nothing that drags in the harness's vcpkg copy.
// See mcp_stdio_shim.hpp for why.

#include "minimal/mcp/mcp_stdio_shim.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <thread>

#include <sys/wait.h>
#include <unistd.h>

#include <mcp_message.h>
#include <mcp_stdio_client.h>

namespace minimal::mcp {

namespace {

// The MCP stdio client writes to the server child's stdin. When the child has
// died, that write raises SIGPIPE, whose default disposition kills the ENTIRE
// server process -- observed as exit 141 mid-test. This ignore used to be the
// process's ONLY guard, installed lazily here -- which meant the hook executor
// and any other pipe-writing code implicitly leaned on whether an unrelated
// stdio MCP server had happened to initialize. The disposition is now set
// unconditionally at the top of server main(); this call stays as an idempotent
// backstop for test binaries that exercise the shim without main(). Children
// spawned afterwards (bash) inherit SIG_IGN, so their pipelines report EPIPE
// errors instead of dying by signal -- the same trade hosted runtimes (Python,
// Go) make, and an improvement in practice: a `yes | head -1` exits with a
// write error it can report rather than a signal it cannot.
void DisableSigpipe() {
    std::signal(SIGPIPE, SIG_IGN);
}

}  // namespace

McpStdioShim::McpStdioShim(const std::string& command_line,
                           const std::map<std::string, std::string>& env)
    : command_line_(command_line), env_(env) {}

McpStdioShim::~McpStdioShim() {
    delete static_cast<::mcp::stdio_client*>(client_.load(std::memory_order_acquire));
}

bool McpStdioShim::Initialize(const std::string& client_name,
                               const std::string& client_version, std::string* error) {
    if (client_.load(std::memory_order_acquire) == nullptr) {
        DisableSigpipe();
        ::mcp::json env_json = ::mcp::json::object();
        for (const auto& [key, value] : env_) env_json[key] = value;
        client_.store(new ::mcp::stdio_client(command_line_, env_json),
                      std::memory_order_release);
    }
    try {
        return static_cast<::mcp::stdio_client*>(client_.load(std::memory_order_acquire))
            ->initialize(client_name, client_version);
    } catch (const ::mcp::mcp_exception& e) {
        if (error != nullptr) *error = e.what();
        return false;
    } catch (const std::exception& e) {
        if (error != nullptr) *error = e.what();
        return false;
    }
}

bool McpStdioShim::Ping() {
    try {
        return static_cast<::mcp::stdio_client*>(client_.load(std::memory_order_acquire))->ping();
    } catch (const ::mcp::mcp_exception&) {
        return false;
    } catch (const std::exception&) {
        return false;
    }
}

std::vector<McpStdioShim::ShimTool> McpStdioShim::ListTools() {
    std::vector<ShimTool> out;
    try {
        for (const auto& tool : static_cast<::mcp::stdio_client*>(client_.load(
                 std::memory_order_acquire))->get_tools()) {
            ShimTool shim;
            shim.name = tool.name;
            shim.description = tool.description;
            shim.schema_json = tool.parameters_schema.dump();
            out.push_back(std::move(shim));
        }
    } catch (const ::mcp::mcp_exception&) {
    } catch (const std::exception&) {
    }
    return out;
}

bool McpStdioShim::CallTool(const std::string& name, const std::string& arguments_json,
                            std::string& result_json, std::string& error) {
    try {
        const ::mcp::json arguments = ::mcp::json::parse(arguments_json);
        const ::mcp::json result = static_cast<::mcp::stdio_client*>(client_.load(
                                       std::memory_order_acquire))->call_tool(name, arguments);
        result_json = result.dump();
        return true;
    } catch (const ::mcp::mcp_exception& e) {
        error = e.what();
        return false;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

int McpStdioShim::AbortChild() {
    if (client_.load(std::memory_order_acquire) == nullptr) return 0;
    const int pid =
        static_cast<::mcp::stdio_client*>(client_.load(std::memory_order_acquire))->child_pid();
    if (pid <= 0) return 0;

    // waitpid(WNOHANG): 0 = alive and still our child (signal it); pid = dead
    // but unreaped (nothing to do); -1 = reaped or never ours (never signal).
    // The check runs at signal time, so a child that already died cannot be
    // confused with a reused pid.
    int status = 0;
    if (waitpid(pid, &status, WNOHANG) != 0) return 0;
    kill(pid, SIGTERM);
    return pid;
}

void McpStdioShim::ReapChild(int pid) {
    if (pid <= 0) return;
    for (int i = 0; i < 50; ++i) {  // 50 * 100 ms = 5 s of SIGTERM grace
        int status = 0;
        const pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid || (result == -1 && errno == ECHILD)) return;  // gone
        if (result == -1) return;  // unexpected errno: nothing more to do
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);  // reap the SIGKILLed child
}

void McpStdioShim::WatchChildDetached(int pid) {
    if (pid <= 0) return;
    std::thread([pid] {
        for (int i = 0; i < 50; ++i) {  // 50 * 100 ms = 5 s of grace
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            int watcher_status = 0;
            const pid_t result = waitpid(pid, &watcher_status, WNOHANG);
            if (result == pid || (result == -1 && errno == ECHILD)) return;  // gone
            if (result == -1) return;  // unexpected errno: nothing more to do
        }
        kill(pid, SIGKILL);
        waitpid(pid, nullptr, 0);  // reap the SIGKILLed child
    }).detach();
}

}  // namespace minimal::mcp
