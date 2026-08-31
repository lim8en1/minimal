#include "minimal/mcp/mcp_tool.hpp"

#include <chrono>

#include "minimal/mcp/naming.hpp"

namespace minimal::mcp {

namespace {

tools::ToolResult ErrorResult(const std::string& message) {
    tools::ToolResult result;
    result.is_error = true;
    result.content_text = message;
    return result;
}

std::string_view PlaceholderFor(std::string_view content_type) {
    if (content_type == "image") return "[image content omitted]";
    if (content_type == "audio") return "[audio content omitted]";
    if (content_type == "resource") return "[embedded resource content omitted]";
    return "[unsupported content type omitted]";
}

}  // namespace

tools::ToolResult McpResultToToolResult(const nlohmann::json& call_result) {
    if (!call_result.is_object()) {
        return ErrorResult("MCP server returned a non-object tool result");
    }

    tools::ToolResult result;
    const bool is_error = call_result.value("isError", false);
    result.is_error = is_error;

    if (!call_result.contains("content") || !call_result.at("content").is_array()) {
        if (is_error) {
            result.content_text = "MCP tool failed (server returned no error detail)";
        } else {
            // A successful call with no content: legal per the protocol
            // (void-style tools). Render as empty text, not as an error.
            result.content_text = "";
        }
        return result;
    }

    std::string text;
    for (const auto& item : call_result.at("content")) {
        if (!item.is_object() || !item.contains("type") || !item.at("type").is_string()) {
            text += "[malformed content item omitted]";
            continue;
        }
        const std::string type = item.at("type").get<std::string>();
        if (type == "text" && item.contains("text") && item.at("text").is_string()) {
            text += item.at("text").get<std::string>();
        } else {
            text += PlaceholderFor(type);
        }
    }
    result.content_text = std::move(text);
    return result;
}

McpTool::McpTool(std::string server_name, llm::ToolDefinition definition,
                 std::shared_ptr<McpInvoker> invoker)
    : server_name_(std::move(server_name)),
      definition_(std::move(definition)),
      invoker_(std::move(invoker)) {
    // definition_ keeps the BARE name: it is what CallTool() sends to the
    // remote server, which does not know about our namespace. Only the spec's
    // copy -- what the model sees and invokes -- carries the namespaced form.
    spec_.name = McpToolName(server_name_, definition_.name);
    spec_.definition = definition_;
    spec_.definition.name = spec_.name;
    spec_.category = tools::ToolCategory::ReadOnly;
}

tools::ToolResult McpTool::execute(const nlohmann::json& input, tools::ToolContext& ctx) {
    auto invoker = invoker_;  // alive even if the manager already re-registered us
    if (!invoker) {
        return ErrorResult("MCP server '" + server_name_ + "' is not connected");
    }

    const auto started = std::chrono::steady_clock::now();
    const McpCallOutcome outcome = invoker->CallTool(definition_.name, input, ctx.cancel_token);
    const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);

    // One audit line per invocation, like bash's. Arguments are NOT logged:
    // they are model-generated and can carry anything; only the shape matters.
    ctx.logger.info("mcp tool '{}' on server '{}': {} ({} ms)",
                    spec_.name, server_name_, outcome.ok ? "ok" : outcome.error,
                    duration.count());

    if (!outcome.ok) {
        return ErrorResult("MCP tool '" + spec_.name + "' failed: " + outcome.error);
    }
    return McpResultToToolResult(outcome.result);
}

}  // namespace minimal::mcp
