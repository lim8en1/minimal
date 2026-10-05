#include "mcp_channels.hpp"

#include <algorithm>
#include <utility>

#include "minimal/common/logger.hpp"
#include "minimal/mcp/mcp_permission_model.hpp"
#include "minimal/mcp/signal_protocol.hpp"

namespace minimal::mcp {

ChannelWiring MakeChannelWiring(config::ChannelsConfig channels, agent::SignalService* signals) {
    ChannelWiring wiring;
    wiring.config = std::move(channels);
    wiring.on_notification = [signals](const std::string& channel, const std::string& raw) {
        // Runs on the channel's read thread: decode, hand over, never block.
        SignalFrame frame;
        std::string error;
        if (!ParseSignalNotification(raw, frame, error)) {
            if (!error.empty()) {
                common::DefaultLogger().warn("signal channel '{}': {}", channel, error);
            }
            return;
        }
        if (signals == nullptr) {
            common::DefaultLogger().warn(
                "signal channel '{}': no signal service in this host; dropped", channel);
            return;
        }
        signals->DeliverExternal(channel, frame.signal, frame.content, frame.meta);
    };
    wiring.on_connected = [signals](const std::string& channel, const std::string& instructions) {
        if (signals != nullptr) signals->SetChannelInstructions(channel, instructions);
    };
    return wiring;
}

void RegisterChannels(const config::ChannelsConfig& channels, McpClientManager& manager,
                      agent::SignalService& signals, McpChannelSource& source,
                      const std::function<void(const std::string&)>& warn) {
    const auto enabled = manager.EnabledChannelNames();
    for (const auto& entry : channels.entries) {
        if (std::find(enabled.begin(), enabled.end(), entry.name) == enabled.end()) continue;
        std::string error;
        if (!signals.RegisterExternalChannel(
                agent::ExternalChannel{entry.name, entry.extended, entry.signals,
                                       std::string(kMcpPluginName)},
                &source,
                error)) {
            warn("signal channel '" + entry.name + "' skipped: " + error);
        }
    }
}

}  // namespace minimal::mcp
