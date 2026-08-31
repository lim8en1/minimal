// Compiled as an ISLAND: include cpp-mcp's headers (which pull their own
// bundled nlohmann/json) and nothing that drags in the harness's vcpkg copy.
// See mcp_stdio_shim.hpp for why.

#include "minimal/mcp/mcp_stdio_shim.hpp"

#include <csignal>

#include <mcp_message.h>
#include <mcp_stdio_client.h>

namespace minimal::mcp {

namespace {

// The MCP stdio client writes to the server child's stdin. When the child has
// died, that write raises SIGPIPE, whose default disposition kills the ENTIRE
// server process -- observed as exit 141 mid-test. Ignore it process-wide: the
// write then fails with EPIPE and surfaces as a tool error, which is the whole
// point of this layer. Children spawned afterwards (bash) inherit SIG_IGN, so
// their pipelines report EPIPE errors instead of dying by signal -- the same
// trade hosted runtimes (Python, Go) make, and an improvement in practice: a
// `yes | head -1` exits with a write error it can report rather than a signal
// it cannot.
void DisableSigpipe() {
    std::signal(SIGPIPE, SIG_IGN);
}

}  // namespace

McpStdioShim::McpStdioShim(const std::string& command_line,
                           const std::map<std::string, std::string>& env)
    : command_line_(command_line), env_(env) {}

McpStdioShim::~McpStdioShim() { delete static_cast<::mcp::stdio_client*>(client_); }

bool McpStdioShim::Initialize(const std::string& client_name,
                              const std::string& client_version) {
    if (client_ == nullptr) {
        DisableSigpipe();
        ::mcp::json env_json = ::mcp::json::object();
        for (const auto& [key, value] : env_) env_json[key] = value;
        client_ = new ::mcp::stdio_client(command_line_, env_json);
    }
    try {
        return static_cast<::mcp::stdio_client*>(client_)->initialize(client_name, client_version);
    } catch (const ::mcp::mcp_exception&) {
        return false;
    } catch (const std::exception&) {
        return false;
    }
}

bool McpStdioShim::Ping() {
    try {
        return static_cast<::mcp::stdio_client*>(client_)->ping();
    } catch (const ::mcp::mcp_exception&) {
        return false;
    } catch (const std::exception&) {
        return false;
    }
}

std::vector<McpStdioShim::ShimTool> McpStdioShim::ListTools() {
    std::vector<ShimTool> out;
    try {
        for (const auto& tool : static_cast<::mcp::stdio_client*>(client_)->get_tools()) {
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
        const ::mcp::json result =
            static_cast<::mcp::stdio_client*>(client_)->call_tool(name, arguments);
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

}  // namespace minimal::mcp
