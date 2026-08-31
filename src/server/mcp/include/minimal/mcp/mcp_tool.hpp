#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "minimal/llm/chat.hpp"
#include "minimal/tools/tool.hpp"

namespace minimal::mcp {

// The one call McpTool makes into the connection layer. Kept as an interface
// rather than a concrete dependency so the adapter (and its conversion logic)
// is testable without a live MCP server -- the same property that makes
// LlmProvider and PermissionChecker cheap to fake.
class McpInvoker {
public:
    virtual ~McpInvoker() = default;

    // Blocks until the call finishes, fails, times out, or `cancel` fires.
    // The connection layer owns deadlines; this interface carries none. A
    // null token is legal: deadline-only, no cancellation path.
    virtual struct McpCallOutcome CallTool(std::string_view tool_name,
                                            const nlohmann::json& arguments,
                                            const tools::CancelTokenPtr& cancel) = 0;
};

// Value-level outcome: a failed MCP call is a tool error, never an exception
// and never a hang. `error` is user-facing text naming what went wrong
// (timeout, dead server, protocol error); it must never contain secrets --
// the connection layer redacts interpolated env before anything reaches here.
struct McpCallOutcome {
    bool ok = false;
    std::string error;     // meaningful iff !ok
    nlohmann::json result; // meaningful iff ok
};

// Converts an MCP tools/call result object into a ToolResult. `content` items
// of type "text" become text; any other content type (image, resource, audio)
// becomes a bracketed placeholder -- there is no multimodal path through
// ToolResult, and dropping the item silently would lose the fact that the
// server returned something. `isError` maps to is_error; a missing content
// array on an error result yields a generic message rather than an empty
// success. Anything that is not an object is an error.
tools::ToolResult McpResultToToolResult(const nlohmann::json& call_result);

// One MCP server tool, registered under `mcp__<server>__<tool>`. Category is
// ReadOnly by declaration: MCP gating is server-scoped (McpPermissionPolicy),
// so the category never feeds the permission decision -- it only shapes
// client display. Path confinement is unsupported: an MCP tool's input schema
// is opaque to us, so out-of-workdir cannot be evaluated (ToolExecutor skips
// the fold for exactly this case).
class McpTool : public tools::Tool {
public:
    // `definition` is the discovery listing (name/description/input_schema);
    // the spec rewrites its name to the namespaced form the model must call.
    // `invoker` is shared: it is the server connection, held alive by this
    // shared_ptr even after the manager has replaced the live registration.
    McpTool(std::string server_name, llm::ToolDefinition definition,
            std::shared_ptr<McpInvoker> invoker);

    const tools::ToolSpec& spec() const override { return spec_; }

    tools::ToolResult execute(const nlohmann::json& input, tools::ToolContext& ctx) override;

    const std::string& server_name() const { return server_name_; }
    const std::string& bare_tool_name() const { return definition_.name; }

private:
    std::string server_name_;
    llm::ToolDefinition definition_;
    std::shared_ptr<McpInvoker> invoker_;
    tools::ToolSpec spec_;
};

}  // namespace minimal::mcp
