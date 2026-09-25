#pragma once

#include <string>
#include <vector>

#include "minimal/mcp/channels_config.hpp"
#include "minimal/mcp/mcps_config.hpp"

namespace minimal::mcp {

// What the operator should hear about mcps.json and channels.json at startup
// (reported through PluginRegistrationContext::report_warning, printed by the
// sanity check): per-entry parse errors, enabled stdio commands that do not
// resolve on PATH, malformed http urls, a channel claiming the reserved
// internal name, and a channel whose name an mcps.json server already holds
// (the channel is skipped). None of these stops the others from working.
std::vector<std::string> ValidateMcpConfig(const config::McpsConfig& servers,
                                           const config::ChannelsConfig& channels);

}  // namespace minimal::mcp
