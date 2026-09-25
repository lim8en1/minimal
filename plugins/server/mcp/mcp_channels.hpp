#pragma once

// The MCP signal channels (channels.json) as an EXTERNAL producer on core's
// signal system (agent/signal_service.hpp): the manager carries the channel
// connections, this file glues them to core.signals' external ingress.
// Core owns delivery -- subscriptions, the allowlist, framing, wake; this
// plugin only decodes the MCP wire frame and hands the router the signal.

#include <functional>
#include <string>
#include <vector>

#include "minimal/agent/signal_service.hpp"
#include "minimal/mcp/mcp_client_manager.hpp"

namespace minimal::mcp {

// Bringing a channel up is bringing its MCP connection up.
class McpChannelSource final : public agent::ExternalChannelSource {
public:
    explicit McpChannelSource(McpClientManager& manager) : manager_(manager) {}
    bool EnsureAlive(const std::string& channel, std::string& error) override {
        return manager_.EnsureAlive(channel, error);
    }

private:
    McpClientManager& manager_;
};

// The manager's ChannelWiring callbacks, feeding `signals` (borrowed; core's
// router outlives the plugin's shutdown). Null `signals` (a host without the
// seam): notifications are dropped, logged once per frame kind.
ChannelWiring MakeChannelWiring(config::ChannelsConfig channels, agent::SignalService* signals);

// Registers with core every channel the manager actually built a handle for
// -- asking the manager, rather than re-deriving its enabled/reserved/
// collision rules, keeps this in sync with EnsureAlive's own view: a
// subscription to anything else would pass the router's selector check and
// then fail, or silently resolve to an unrelated server handle of the same
// name. Refusals are reported through `warn`.
void RegisterChannels(const config::ChannelsConfig& channels, McpClientManager& manager,
                      agent::SignalService& signals, McpChannelSource& source,
                      const std::function<void(const std::string&)>& warn);

}  // namespace minimal::mcp
