#include "mcp_settings.hpp"

#include <chrono>

#include "minimal/common/config_reader.hpp"
#include "minimal/common/clock.hpp"
#include "minimal/common/plugin_config_label.hpp"

namespace minimal::mcp {

McpLimits ParseMcpLimits(const nlohmann::json& config) {
    McpLimits limits;
    constexpr std::int64_t kMaxMs = common::kMaxDeadline.count();
    config::Reader reader(config, common::PluginConfigLabel(common::kPluginConfigDirName, "mcp"));
    reader.Describe("connect_timeout_ms", {"Bound on connecting to one MCP server."});
    reader.Describe("request_timeout_ms", {"Bound on one MCP request."});
    reader.Describe("max_list_pages", {"Page cap when listing a server's tools."});
    reader.Describe("restart_cooldown_ms", {"Wait before restarting a server that exited."});
    limits.connect = std::chrono::milliseconds(
        reader.Int("connect_timeout_ms", limits.connect.count(), 1, kMaxMs));
    limits.request = std::chrono::milliseconds(
        reader.Int("request_timeout_ms", limits.request.count(), 1, kMaxMs));
    limits.max_list_pages = static_cast<decltype(limits.max_list_pages)>(
        reader.Int("max_list_pages", limits.max_list_pages, 1));
    limits.restart_cooldown = std::chrono::milliseconds(
        reader.Int("restart_cooldown_ms", limits.restart_cooldown.count(), 0, kMaxMs));
    reader.Finish();
    return limits;
}

}  // namespace minimal::mcp
