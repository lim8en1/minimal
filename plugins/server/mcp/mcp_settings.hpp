#pragma once

#include <nlohmann/json.hpp>

#include "minimal/mcp/mcp_connection.hpp"

namespace minimal::mcp {

// The manager's limits, from this plugin's plugins.json `config` object:
//
//   {"file": "mcp.so", "config": {"connect_timeout_ms": 10000,
//                                 "request_timeout_ms": 60000,
//                                 "max_list_pages": 100,
//                                 "restart_cooldown_ms": 30000}}
//
// An absent key keeps the McpLimits default.
// Throws on an invalid value (max_list_pages < 1, a negative timeout or
// cooldown) -- a bundled plugin's registration failure, fatal at startup,
// like core's own settings errors were.
McpLimits ParseMcpLimits(const nlohmann::json& config);

}  // namespace minimal::mcp
