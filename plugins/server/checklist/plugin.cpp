// checklist -- the model's per-session task checklist.
//
// Three seams, one owner: the `checklist` TOOL writes the model's copy of the
// list, the "checklist.rpc" SERVICE serves the same state to UI consumers over
// the generic PluginCall channel, and both share the ChecklistStore below.
// A plugin owning the state it touches, and the command-free version of the
// tools-bash shape.
//
// VIEW SURFACE: deliberately NOT server-rendered. The tool result text is what
// the MODEL sees (the model has no other medium); UI presentation is a client
// concern and fetches structured items through the .rpc service -- the same
// unary RPC a web or mobile frontend would call. Baking presentation into the
// server would make every other frontend re-parse rendered markdown or go
// without.
//
// STATE: one list per session in this plugin's storage namespace (keyed by
// session id). It survives session termination so a resumed session keeps its
// list, and is deliberately NOT purged on OnSessionEnded: rows are tiny, there
// is no session-deleted signal, and purging on end would break resume.
//
// DEGRADE: context.storage may be null (a host without the seam). The tool
// then answers every op with a clear error result and the service replies
// !ok -- a runtime condition, not a crash.

#include "minimal/plugins/plugin_abi.h"

#include <memory>

#include "checklist_store.hpp"
#include "checklist_tool.hpp"
#include "rpc_service.hpp"

namespace {

// One store for the whole plugin: the tool and the .rpc service operate on the
// same state. Process-lifetime static (plugins are never unloaded), holding a
// possibly-null storage pointer captured at registration.
std::shared_ptr<minimal::checklist::ChecklistStore>& SharedStore() {
    static std::shared_ptr<minimal::checklist::ChecklistStore> store;
    return store;
}

minimal::checklist::ChecklistRpcService& SharedRpcService() {
    static minimal::checklist::ChecklistRpcService service(SharedStore());
    return service;
}

}  // namespace

extern "C" {

MINIMAL_PLUGIN_ABI_SYMBOLS(minimal::checklist::kPluginName)

void minimal_plugin_register_tools(
    minimal::tools::ToolRegistry& registry,
    const minimal::plugins::PluginRegistrationContext& context) {
    SharedStore() = std::make_shared<minimal::checklist::ChecklistStore>(context.storage);
    registry.Register(minimal::checklist::MakeChecklistTool(SharedStore()));
}

// The PluginCall dispatch target, under the "<plugin_name>.rpc" convention.
// Registered through the same seam any third-party service uses; core's
// PluginCall half routes to it by name at call time.
void minimal_plugin_register_services(
    minimal::plugins::ServiceRegistry& registry,
    const minimal::plugins::PluginRegistrationContext&) {
    // The loader invokes tools registration before services registration, so
    // the store exists by now; this guard is belt-and-braces for a
    // sessionless/toolless host that skips tool registration entirely.
    // ChecklistRpcService binds to SharedStore()'s SLOT by reference (not a
    // copy of its value), so even if this guard ever ran too late, a later
    // write here or from the tool registration path is still visible to the
    // service -- unlike the by-value capture this replaced.
    if (!SharedStore()) SharedStore() = std::make_shared<minimal::checklist::ChecklistStore>(nullptr);
    registry.Provide(minimal::plugins::PluginRpcServiceName(minimal::checklist::kPluginName),
                     &SharedRpcService());
}

}  // extern "C"
