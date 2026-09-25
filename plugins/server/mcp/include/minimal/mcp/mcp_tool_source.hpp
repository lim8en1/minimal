#pragma once

#include <optional>
#include <string>
#include <vector>

#include "minimal/common/logger.hpp"
#include "minimal/mcp/mcp_client_manager.hpp"
#include "minimal/tools/tool_source.hpp"

namespace minimal::mcp {

// The MCP servers as a tool source (tools/tool_source.hpp): members are the
// mcps.json servers, activating one brings its shared connection up and
// answers its tool group ("mcp__<server>__").
inline constexpr const char* kMcpToolSourceName = "mcp";

class McpToolSource final : public tools::ToolSource {
public:
    McpToolSource(McpClientManager& manager, common::Logger& logger)
        : manager_(manager), logger_(logger) {}

    std::vector<std::string> Members() const override { return manager_.ToolServerNames(); }

    std::optional<std::string> Activate(const std::string& member, std::string& error) override;

private:
    McpClientManager& manager_;
    common::Logger& logger_;
};

}  // namespace minimal::mcp
