#pragma once

// The calling convention for the generic PluginCall channel.
//
// The proto is core and frozen: plugins never add RPCs to it ("a bespoke core
// RPC per plugin would defeat the purpose"). Instead there is exactly one
// generic channel -- PluginCall{plugin_name, method, json_payload} -- and this
// header is the contract its server half dispatches THROUGH. Core defines the
// convention (as it does for Tool and Command) but neither payload nor
// semantics: what a method means is the providing plugin's business, and every
// consumer -- the TUI via a client-plugin view, a web or mobile frontend over
// plain gRPC, a script -- calls the same unary RPC and reads the same JSON.
//
// DISPATCH CONVENTION: a plugin serving UI consumers provides a handler under
// PluginRpcServiceName("<plugin_name>") -- "<plugin_name>.rpc". The .rpc
// suffix keeps the dispatch target out of the way of plain §2.2 services the
// same plugin may also provide (ServiceRegistry names feed NameRegistry as
// NameKind::Service, so a collision would be fatal; the suffix makes one
// unexpressible). Resolution is lazy at call time through
// ServiceRegistry::Find: null means the plugin is disabled or absent, a
// runtime condition the transport maps to NOT_FOUND -- never an error.
//
// THREADING: Handle runs on gRPC handler threads, concurrently across calls.
// The registry is sealed before serving, so Find is safe; the handler's own
// state must be. The checklist store is the reference shape: all mutable state
// behind PluginStorage's per-call mutex.

#include <string>
#include <string_view>

#include "minimal/plugins/service_registry.hpp"

namespace minimal::plugins {

// "<plugin_name>.rpc" -- the ServiceRegistry name a plugin's PluginCall
// dispatch target is provided under.
inline std::string PluginRpcServiceName(std::string_view plugin_name) {
    return std::string(plugin_name) + ".rpc";
}

class PluginRpcHandler {
public:
    virtual ~PluginRpcHandler() = default;

    struct Reply {
        // False = a plugin-domain failure (unknown method, malformed payload,
        // no such resource). The transport surfaces `error` to the caller and
        // the plugin should put anything structured it wants the UI to show
        // INTO successful payloads -- a Reply carries no side channel.
        bool ok = true;
        // The response JSON, plugin-defined, passed through verbatim.
        std::string payload;
        // REQUIRED when !ok; ignored otherwise.
        std::string error;
    };

    // One method call. `json_payload` is whatever the caller sent ("" when the
    // method takes no arguments) -- parsing it, and rejecting what it gets
    // wrong via {!ok, error}, is the handler's job.
    virtual Reply Handle(const std::string& method, std::string_view json_payload) = 0;
};

// What a dispatch produced, grpc-free so it is unit-testable without a
// channel. The transport maps these onto statuses:
//   kOk       -> OK, payload returned
//   kNotFound -> NOT_FOUND (no such plugin, or it provides no .rpc service)
//   kInvalid  -> INVALID_ARGUMENT (empty plugin_name/method, handler refused)
//   kInternal -> INTERNAL (the handler threw)
enum class RpcOutcome { kOk, kNotFound, kInvalid, kInternal };

struct RpcCallResult {
    RpcOutcome outcome = RpcOutcome::kOk;
    std::string payload;  // kOk: the handler's payload; otherwise ""
    std::string error;    // kInvalid/kInternal: a message; otherwise ""
};

// Resolves PluginRpcServiceName(plugin_name) through `services`, calls the
// handler, and classifies the result. `services` may be null (host without the
// seam) and the resolved pointer may be null (plugin disabled) -- both are
// kNotFound, the "runtime condition, not an error" contract.
RpcCallResult DispatchPluginCall(const ServiceRegistry* services, std::string_view plugin_name,
                                 std::string_view method, std::string_view json_payload);

}  // namespace minimal::plugins
