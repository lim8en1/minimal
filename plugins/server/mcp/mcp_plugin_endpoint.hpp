#pragma once

// The client channel's endpoint, Provided as "mcp.endpoint" (plugin_abi.h's
// PluginEndpoint convention) -- what the mcp-view client plugin's picker
// talks to. Unary only:
//
//   list         {}                            -> {"servers": [{"name", "transport",
//                                                   "configured", "enabled", "alive",
//                                                   "error"}]}   (every handle -- mcps.json
//                                                   servers and channels -- sorted)
//   set_enabled  {"name": s, "enabled": b}      -> {"ok": b, "error": s}
//
// Subscribe refuses every method: the picker refreshes after its own toggle.

#include <string>

#include "minimal/mcp/mcp_client_manager.hpp"
#include "minimal/plugins/plugin_abi.h"

namespace minimal::mcp {

class McpPluginEndpoint final : public plugins::PluginEndpoint {
public:
    // Borrowed; the plugin owns it until shutdown.
    explicit McpPluginEndpoint(McpClientManager& manager) : manager_(manager) {}

    std::string Call(const std::string& method, const std::string& json_payload) override;

    bool Subscribe(const std::string&, const std::string&,
                   std::shared_ptr<plugins::PluginSubscriptionSink>) override {
        return false;
    }

private:
    McpClientManager& manager_;
};

}  // namespace minimal::mcp
