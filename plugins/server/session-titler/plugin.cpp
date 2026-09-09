// session-titler -- the ARCHITECTURE.md "Session naming, layered" second
// layer, as an observer plugin exactly as designed there. Ships disabled
// (DefaultsDisabled in plugin_discovery.cpp): it is the one bundled plugin
// that spends the user's tokens unprompted, so enabling it is a decision.

#include "minimal/agent/session_observer_registry.hpp"
#include "minimal/common/abi_tag.hpp"
#include "minimal/plugins/plugin_abi.h"

#include "titler.hpp"

extern "C" {

MINIMAL_PLUGIN_ABI_SYMBOLS("session-titler")

void minimal_plugin_register_session_observer(
    minimal::agent::SessionObserverRegistry& registry,
    const minimal::plugins::PluginRegistrationContext& context) {
    auto config = minimal::session_titler::ParseTitlerConfig(context.config);
    // The ServiceRegistry pointer is captured at registration and resolved
    // lazily (Find at titling time) per the service contract -- this plugin
    // may load before or after any other, and the registry is sealed long
    // before the first session exists.
    registry.Register(std::make_shared<minimal::session_titler::TitlerObserver>(
        std::move(config), context.services, minimal::common::DefaultLogger()));
}

}  // extern "C"
