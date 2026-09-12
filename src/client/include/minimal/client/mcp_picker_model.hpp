#pragma once

// Pure row-building logic for the /mcp picker, the same "(data, filter) ->
// rows over a snapshot value type, no FTXUI, no AppState, no network" shape
// model_picker_model.hpp uses.

#include <string>
#include <vector>

#include "minimal/v1/session_service.pb.h"

namespace minimal::client {

struct McpPickerRow {
    std::string label;
    std::string name;  // the server name -- empty is never valid here (no header rows)
};

// A flat list (no provider-style grouping -- a server list is small and has
// no natural 2-tier axis). The label carries the server's live status so a
// glance says enabled/disabled and alive/dead without opening anything:
// "name [off]", "name [on, connected]", "name [on, error: ...]". An empty
// filter lists every server; a non-empty one matches the name
// case-insensitively.
std::vector<McpPickerRow> BuildMcpPickerRows(const std::vector<v1::McpServerSummary>& servers,
                                             const std::string& filter);

}  // namespace minimal::client
