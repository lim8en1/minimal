// Registers one hook point, the way tools-bash registers before_command.
// The point name is plugin-qualified (the services convention); the handle is
// captured so the loader test can verify record.Registered("hook") and the
// registration-order guarantee (points before tools) -- exposed to the test
// through the service channel, since a dlopen'd plugin's symbols are private.
#include "minimal/plugins/plugin_abi.h"

namespace {

minimal::hooks::HookPoint& Point() {
    static minimal::hooks::HookPoint point;
    return point;
}

bool g_captured_before_tools = false;

}  // namespace

extern "C" {
const char* minimal_plugin_abi_tag() { return minimal::common::AbiTag(); }
const char* minimal_plugin_name() { return "fixture-hook"; }
const char* minimal_plugin_version() { return "1.0.0"; }

void minimal_plugin_register_hook_points(
    minimal::hooks::HookDispatcher& registry,
    const minimal::plugins::PluginRegistrationContext&) {
    Point() = registry.RegisterPoint({"fixture-hook.before_thing", /*veto_capable=*/true,
                                      /*filterable=*/{"thing_id"}});
}

// Called after register_hook_points by the loader; records whether the handle
// was already minted (enabled) when tools register -- the ordering guarantee
// the worked example (tools-bash) relies on.
void minimal_plugin_register_tools(
    minimal::tools::ToolRegistry& registry,
    const minimal::plugins::PluginRegistrationContext&) {
    (void)registry;
    g_captured_before_tools = Point().enabled();
}

void minimal_plugin_register_services(
    minimal::plugins::ServiceRegistry& registry,
    const minimal::plugins::PluginRegistrationContext&) {
    registry.Provide("fixture.hook_captured_before_tools", &g_captured_before_tools);
}

}  // extern "C"
