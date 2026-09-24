#pragma once

// The plugin's unary methods, provided as "<plugin_name>.rpc" (see
// minimal/plugins/plugin_rpc.hpp) and answered verbatim by the endpoint's
// Call (endpoint.hpp). This is the UI data path: any consumer -- a TUI
// client-plugin view, a web or mobile frontend, a script -- fetches
// checklist state through the generic PluginCall RPC and reads
// plugin-defined JSON. Core transports and never interprets it.
//
// Methods (JSON in, JSON out):
//   get  {"session_id": "..."}  -> {"session_id": "...", "items":[...]}
//   list {"session_ids": [...]} -> {"checklists":[{"session_id": "...", "items":[...]}]}
//        only the named sessions that have a stored list; `session_ids`
//        omitted -> every stored list.
//
// Prefer the filtered `list`: the store keeps every session's list forever
// (checklist_store.hpp), so the unfiltered form grows without bound. It
// cannot filter by project_root on its own: the plugin holds no session
// metadata -- a UI that needs the join calls the core ListSessions RPC
// itself and then lists the ids it got.
//
// Threading: Handle runs on gRPC handler threads; all state sits behind
// PluginStorage's per-call mutex.

#include "checklist_store.hpp"
#include "minimal/plugins/plugin_rpc.hpp"

namespace minimal::checklist {

class ChecklistRpcService : public plugins::PluginRpcHandler {
public:
    // Borrowed; process lifetime.
    explicit ChecklistRpcService(ChecklistStore& store) : store_(store) {}

    Reply Handle(const std::string& method, std::string_view json_payload) override;

private:
    ChecklistStore& store_;
};

}  // namespace minimal::checklist
