#include "mcp_settings.hpp"

#include <stdexcept>
#include <string>

namespace minimal::mcp {
namespace {

std::int64_t Knob(const nlohmann::json& config, const char* key, std::int64_t fallback) {
    if (config.is_object() && config.contains(key)) return config.at(key).get<std::int64_t>();
    return fallback;
}

}  // namespace

McpLimits ParseMcpLimits(const nlohmann::json& config) {
    McpLimits limits;
    try {
        const auto connect = Knob(config, "connect_timeout_ms", limits.connect.count());
        const auto request = Knob(config, "request_timeout_ms", limits.request.count());
        const auto pages = Knob(config, "max_list_pages", limits.max_list_pages);
        const auto cooldown =
            Knob(config, "restart_cooldown_ms", limits.restart_cooldown.count());
        if (connect < 0 || request < 0) {
            throw std::runtime_error("mcp: connect/request timeouts must not be negative");
        }
        if (pages < 1) throw std::runtime_error("mcp: max_list_pages must be at least 1");
        if (cooldown < 0) throw std::runtime_error("mcp: restart_cooldown_ms must not be negative");
        limits.connect = std::chrono::milliseconds(connect);
        limits.request = std::chrono::milliseconds(request);
        limits.max_list_pages = pages;
        limits.restart_cooldown = std::chrono::milliseconds(cooldown);
    } catch (const nlohmann::json::exception& e) {
        throw std::runtime_error(std::string("mcp: limits must be integers: ") + e.what());
    }
    return limits;
}

}  // namespace minimal::mcp
