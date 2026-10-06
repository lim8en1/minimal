#include "mcp_settings.hpp"

#include "minimal/common/config_reader.hpp"
#include "minimal/common/plugin_config_label.hpp"

namespace minimal::mcp {

McpLimits ParseMcpLimits(const nlohmann::json& config) {
    McpLimits limits;
    constexpr std::int64_t kMaxMs = 24LL * 3600 * 1000;
    config::Reader reader(config, common::PluginConfigLabel(common::kPluginConfigDirName, "mcp"));
    limits.connect = std::chrono::milliseconds(
        reader.Int("connect_timeout_ms", limits.connect.count(), 1, kMaxMs));
    limits.request = std::chrono::milliseconds(
        reader.Int("request_timeout_ms", limits.request.count(), 1, kMaxMs));
    limits.max_list_pages = static_cast<decltype(limits.max_list_pages)>(
        reader.Int("max_list_pages", limits.max_list_pages, 1, 100000));
    limits.restart_cooldown = std::chrono::milliseconds(
        reader.Int("restart_cooldown_ms", limits.restart_cooldown.count(), 0, kMaxMs));
    reader.Finish();
    return limits;
}

}  // namespace minimal::mcp
