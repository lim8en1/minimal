#include "mcp_picker_model.hpp"

#include <algorithm>

#include <nlohmann/json.hpp>

#include "minimal/client/utf8.hpp"

namespace minimal::mcp_view {

std::optional<std::vector<McpServerStatus>> ParseServerList(const std::string& json) {
    const auto doc = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
    if (!doc.is_object() || !doc.contains("servers") || !doc.at("servers").is_array()) {
        return std::nullopt;
    }
    std::vector<McpServerStatus> out;
    for (const auto& server : doc.at("servers")) {
        if (!server.is_object()) continue;
        McpServerStatus status;
        status.name = server.value("name", std::string());
        if (status.name.empty()) continue;
        status.transport = server.value("transport", std::string("stdio"));
        status.enabled = server.value("enabled", false);
        status.alive = server.value("alive", false);
        status.error = server.value("error", std::string());
        out.push_back(std::move(status));
    }
    return out;
}

std::vector<McpPickerRow> BuildMcpPickerRows(const std::vector<McpServerStatus>& servers,
                                             const std::string& filter) {
    const std::string lowered_filter = client::ToLower(filter);

    std::vector<const McpServerStatus*> ordered;
    ordered.reserve(servers.size());
    for (const auto& server : servers) ordered.push_back(&server);
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const McpServerStatus* a, const McpServerStatus* b) {
                         return a->name < b->name;
                     });

    std::vector<McpPickerRow> rows;
    for (const auto* server : ordered) {
        if (!lowered_filter.empty() &&
            client::ToLower(server->name).find(lowered_filter) == std::string::npos) {
            continue;
        }
        std::string label = server->name + "  [" + server->transport + ", ";
        if (!server->enabled) {
            label += "disabled]";
        } else if (server->alive) {
            label += "enabled, connected]";
        } else if (!server->error.empty()) {
            label += "enabled, error: " + server->error + "]";
        } else {
            label += "enabled, not yet connected]";
        }
        rows.push_back(McpPickerRow{std::move(label), server->name});
    }
    return rows;
}

}  // namespace minimal::mcp_view
