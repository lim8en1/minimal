// checklist -- the model's per-session task checklist.
//
// Four seams, one owner, one store: the `checklist` TOOL writes the model's
// copy of the list; the "checklist.endpoint" serves the same state to UI
// consumers over the generic PluginCall/PluginSubscribe channel -- unary
// get/list, plus a "changes" push stream fed by the tool's writes; the
// ".rpc" service carries the same unary methods for other consumers; and a
// session observer exists only so its destructor joins the push hub's worker
// at teardown.
//
// VIEW SURFACE: deliberately NOT server-rendered. The tool result text is what
// the MODEL sees (the model has no other medium); UI presentation is a client
// concern and gets structured items through the endpoint -- the same channel
// a web or mobile frontend would use. Baking presentation into the server
// would make every other frontend re-parse rendered markdown or go without.
//
// STATE: one list per session in this plugin's storage namespace (keyed by
// session id). It survives session termination so a resumed session keeps its
// list, and is deliberately NOT purged on OnSessionEnded: rows are tiny, there
// is no session-deleted signal, and purging on end would break resume.
//
// DEGRADE: context.storage may be null (a host without the seam). The tool
// then answers every op with a clear error result and the endpoint refuses
// every call -- a runtime condition, not a crash.

#include "minimal/plugins/plugin_abi.h"

#include <memory>

#include "checklist_hub.hpp"
#include "checklist_store.hpp"
#include "checklist_tool.hpp"
#include "endpoint.hpp"
#include "minimal/agent/session_observer_registry.hpp"
#include "rpc_service.hpp"

namespace {

// One set of process-lifetime statics (plugins are never unloaded), built at
// whichever hook runs first from that hook's context -- every hook's context
// carries the same storage pointer, so registration order cannot matter.
std::shared_ptr<minimal::checklist::ChecklistStore>& SharedStore() {
    static std::shared_ptr<minimal::checklist::ChecklistStore> store;
    return store;
}

std::shared_ptr<minimal::checklist::ChecklistRpcService>& SharedRpc() {
    static std::shared_ptr<minimal::checklist::ChecklistRpcService> rpc;
    return rpc;
}

// The push hub. Its worker reads the store, so Shutdown() runs from
// ChecklistHubOwner's destructor, never from a static destructor after
// plugin_storage is gone.
std::shared_ptr<minimal::checklist::ChecklistHub>& SharedHub() {
    static std::shared_ptr<minimal::checklist::ChecklistHub> hub;
    return hub;
}

void EnsureSharedState(const minimal::plugins::PluginRegistrationContext& context) {
    if (!SharedStore()) {
        SharedStore() = std::make_shared<minimal::checklist::ChecklistStore>(context.storage);
    }
    if (!SharedRpc()) {
        SharedRpc() = std::make_shared<minimal::checklist::ChecklistRpcService>(*SharedStore());
    }
    if (!SharedHub()) {
        SharedHub() = std::make_shared<minimal::checklist::ChecklistHub>(*SharedStore());
    }
}

}  // namespace

extern "C" {

MINIMAL_PLUGIN_ABI_SYMBOLS(minimal::checklist::kPluginName)

void minimal_plugin_register_tools(
    minimal::tools::ToolRegistry& registry,
    const minimal::plugins::PluginRegistrationContext& context) {
    EnsureSharedState(context);
    registry.Register(minimal::checklist::MakeChecklistTool(SharedStore(), SharedHub().get()));
}

void minimal_plugin_register_session_observer(
    minimal::agent::SessionObserverRegistry& registry,
    const minimal::plugins::PluginRegistrationContext& context) {
    EnsureSharedState(context);
    registry.Register(std::make_shared<minimal::checklist::ChecklistHubOwner>(*SharedHub()));
}

void minimal_plugin_register_services(
    minimal::plugins::ServiceRegistry& registry,
    const minimal::plugins::PluginRegistrationContext& context) {
    EnsureSharedState(context);
    registry.Provide(minimal::plugins::PluginRpcServiceName(minimal::checklist::kPluginName),
                     SharedRpc().get());

    // The UI data path: PluginCall (same methods as .rpc, which the dispatch
    // now reaches through here) plus the PluginSubscribe "changes" stream.
    static minimal::checklist::ChecklistEndpoint endpoint(*SharedRpc(), *SharedHub());
    registry.Provide(
        minimal::plugins::PluginEndpointServiceName(minimal::checklist::kPluginName), &endpoint);
}

}  // extern "C"
