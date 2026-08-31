#include "minimal/plugins/plugin_rpc.hpp"

#include "minimal/common/logger.hpp"

namespace minimal::plugins {

RpcCallResult DispatchPluginCall(const ServiceRegistry* services, std::string_view plugin_name,
                                 std::string_view method, std::string_view json_payload) {
    RpcCallResult result;

    if (plugin_name.empty()) {
        result.outcome = RpcOutcome::kInvalid;
        result.error = "plugin_name is required";
        return result;
    }
    if (method.empty()) {
        result.outcome = RpcOutcome::kInvalid;
        result.error = "method is required";
        return result;
    }

    const auto service_name = PluginRpcServiceName(plugin_name);
    // Find returning null is the documented contract for "disabled or absent"
    // -- a runtime condition the transport reports, not a failure to hide.
    auto* handler =
        static_cast<PluginRpcHandler*>(services ? services->Find(service_name) : nullptr);
    if (handler == nullptr) {
        result.outcome = RpcOutcome::kNotFound;
        result.error = "plugin '" + std::string(plugin_name) +
                       "' does not provide an RPC service";
        return result;
    }

    try {
        const auto reply = handler->Handle(std::string(method), json_payload);
        result.outcome = reply.ok ? RpcOutcome::kOk : RpcOutcome::kInvalid;
        result.payload = std::move(reply.payload);
        result.error = std::move(reply.error);
        return result;
    } catch (const std::exception& error) {
        // Plugin code is arbitrary native code by design; a throw is an
        // outcome like any other. INTERNAL, named, and the process lives.
        common::DefaultLogger().warn("plugin rpc {}/{} threw: {}", plugin_name, method,
                                     error.what());
        result.outcome = RpcOutcome::kInternal;
        result.error = error.what();
        return result;
    } catch (...) {
        common::DefaultLogger().warn("plugin rpc {}/{} threw a non-standard exception",
                                     plugin_name, method);
        result.outcome = RpcOutcome::kInternal;
        result.error = "plugin '" + std::string(plugin_name) + "' handler threw a non-standard "
                       "exception";
        return result;
    }
}

}  // namespace minimal::plugins
