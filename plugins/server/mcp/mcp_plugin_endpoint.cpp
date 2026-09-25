#include "mcp_plugin_endpoint.hpp"

#include <algorithm>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace minimal::mcp {

std::string McpPluginEndpoint::Call(const std::string& method, const std::string& json_payload) {
    if (method == "list") {
        auto names = manager_.ServerNames();
        std::sort(names.begin(), names.end());
        nlohmann::json servers = nlohmann::json::array();
        for (const auto& name : names) {
            const auto status = manager_.StatusOf(name);
            servers.push_back({{"name", name},
                               {"transport",
                                status.transport == config::McpTransport::Http ? "http" : "stdio"},
                               {"configured", status.configured},
                               {"enabled", status.enabled},
                               {"alive", status.alive},
                               {"error", status.error}});
        }
        return nlohmann::json{{"servers", std::move(servers)}}.dump();
    }
    if (method == "set_enabled") {
        const auto payload = nlohmann::json::parse(json_payload.empty() ? "{}" : json_payload);
        const auto name = payload.value("name", std::string());
        const bool enabled = payload.value("enabled", true);
        if (name.empty()) throw std::invalid_argument("set_enabled needs a server name");
        std::string error;
        const bool ok = manager_.SetEnabled(name, enabled, error);
        return nlohmann::json{{"ok", ok}, {"error", error}}.dump();
    }
    throw std::invalid_argument("unknown mcp method '" + method + "'");
}

}  // namespace minimal::mcp
