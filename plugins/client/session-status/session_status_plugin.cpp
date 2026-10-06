// session-status -- the status bar's usual segments (session_status.hpp).

#include "minimal/client/plugins/client_plugin_abi.h"
#include "session_status.hpp"

namespace plugins = minimal::client::plugins;

extern "C" {

MINIMAL_CLIENT_PLUGIN_ABI_SYMBOLS("session-status")

void minimal_client_plugin_register_status_segments(
    plugins::StatusSegmentRegistry& registry, const plugins::ClientPluginRegistrationContext&) {
    for (auto& segment : minimal::session_status::MakeSegments()) {
        registry.Register(std::move(segment));
    }
}

}  // extern "C"
