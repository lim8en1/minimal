#pragma once

// The plugin's PluginCall dispatch target, provided as "<plugin_name>.rpc"
// (see minimal/plugins/plugin_rpc.hpp for the convention). This is the UI data
// path: any consumer -- a TUI client-plugin view, a web or mobile frontend, a
// script -- fetches checklist state through the generic unary PluginCall RPC
// and reads plugin-defined JSON. Core transports and never interprets it.
//
// Methods (JSON in, JSON out):
//   get  {"session_id": "..."}  -> {"session_id": "...", "items":[...]}
//   list {}                     -> {"checklists":[{"session_id": "...", "items":[...]}]}
//
// `list` enumerates this plugin's storage keys, which are session ids. It
// cannot filter by project_root on its own: the plugin holds no session
// metadata -- a UI that needs the join calls the core ListSessions RPC itself
// and then gets per session.
//
// Threading: Handle runs on gRPC handler threads; all state sits behind
// PluginStorage's per-call mutex.

#include "checklist_store.hpp"
#include "minimal/plugins/plugin_rpc.hpp"

namespace minimal::checklist {

class ChecklistRpcService : public plugins::PluginRpcHandler {
public:
    explicit ChecklistRpcService(std::shared_ptr<ChecklistStore> store);

    Reply Handle(const std::string& method, std::string_view json_payload) override;

private:
    std::shared_ptr<ChecklistStore> store_;
};

}  // namespace minimal::checklist
