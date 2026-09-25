// Exercises the lifecycle seams: report_warning at registration and
// minimal_plugin_shutdown at teardown. The shutdown count is exposed through
// the service channel, since a dlopen'd plugin's symbols are private.
#include "minimal/plugins/plugin_abi.h"

namespace {
int g_shutdowns = 0;
}  // namespace

extern "C" {
MINIMAL_PLUGIN_ABI_SYMBOLS("fixture-lifecycle")

void minimal_plugin_register_tools(minimal::tools::ToolRegistry&,
                                   const minimal::plugins::PluginRegistrationContext& context) {
    if (context.report_warning) context.report_warning("fixture config is odd");
}

void minimal_plugin_register_services(minimal::plugins::ServiceRegistry& registry,
                                      const minimal::plugins::PluginRegistrationContext&) {
    registry.Provide("fixture-lifecycle.shutdowns", &g_shutdowns);
}

void minimal_plugin_shutdown() { ++g_shutdowns; }
}  // extern "C"
