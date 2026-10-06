#include "mcp_settings.hpp"

#include <chrono>

#include "minimal/common/config_reader.hpp"
#include "minimal/common/clock.hpp"

namespace minimal::mcp {

McpLimits ParseMcpLimits(const nlohmann::json& config) {
    McpLimits limits;
    constexpr std::int64_t kMaxMs = common::kMaxDeadline.count();
    config::Reader reader(config, "plugins.json", "mcp.config");
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
