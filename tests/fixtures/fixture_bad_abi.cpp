// Reports a deliberately wrong ABI tag. Must be skipped, never executed --
// a plugin built against a different toolchain must not reach our types.
#include "minimal/plugins/plugin_abi.h"

namespace { bool g_registration_ran = false; }

extern "C" {
const char* minimal_plugin_abi_tag() { return "definitely-not-this-toolchain"; }
const char* minimal_plugin_name() { return "fixture-bad-abi"; }
const char* minimal_plugin_version() { return "1.0.0"; }

// If the loader ever calls this, the ABI gate failed open.
void minimal_plugin_register_tools(minimal::tools::ToolRegistry&,
                                   const minimal::plugins::PluginRegistrationContext&) {
    g_registration_ran = true;
}
}

