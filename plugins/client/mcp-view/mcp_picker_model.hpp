#pragma once

// Pure row-building logic for the /mcp picker, the same "(data, filter) ->
// rows over a snapshot value type, no FTXUI, no network" shape
// model_picker_model.hpp uses. The data is the server-side mcp plugin's
// "mcp.endpoint" `list` answer (plugins/server/mcp/mcp_plugin_endpoint.hpp).

#include <optional>
#include <string>
#include <vector>

namespace minimal::mcp_view {

struct McpServerStatus {
    std::string name;
    std::string transport;  // "stdio" | "http"
    bool enabled = false;
    bool alive = false;
    std::string error;
};

// Decodes a `list` answer ({"servers": [...]}). nullopt: not that shape.
std::optional<std::vector<McpServerStatus>> ParseServerList(const std::string& json);

struct McpPickerRow {
    std::string label;
    std::string name;  // the server name -- empty is never valid here (no header rows)
};

// A flat list (a server list is small and has no natural 2-tier axis). The
// label carries the server's transport and live status so a glance says
// enabled/disabled and alive/dead without opening anything: "name
// [transport, disabled]", "name  [transport, enabled, connected]", "name
// [transport, enabled, error: ...]", "name  [transport, enabled, not yet
// connected]". An empty filter lists every server; a non-empty one matches
// the name case-insensitively. Sorted by name.
std::vector<McpPickerRow> BuildMcpPickerRows(const std::vector<McpServerStatus>& servers,
                                             const std::string& filter);

}  // namespace minimal::mcp_view
