// Serves the generic client channel (PluginCall / PluginSubscribe): Provides a
// PluginEndpoint under the plugin's own name, which the RPC layer dispatches
// to by plugin_name. The pair proof's SERVER half -- the client half is a
// SessionClient::PluginCall/PluginSubscribe caller.
//
// Contract (also asserted by the wire tests):
//   Call("ping", payload)           -> {"pong": <payload verbatim>}
//   Call("echo.<suffix>", payload)  -> <payload> + "/" + <suffix>
//   Call("boom", ...)               -> throws (the RPC answers INTERNAL)
//   Subscribe("ticks", <count>)     -> pushes "tick-0".."tick-<count-1>",
//                                      stopping early if the sink says stop
#include "minimal/plugins/plugin_abi.h"

#include <atomic>
#include <thread>

namespace {

namespace plugins = minimal::plugins;

class FixtureEndpoint final : public plugins::PluginEndpoint {
public:
    std::string Call(const std::string& method, const std::string& json_payload) override {
        if (method == "ping") return "{\"pong\":" + json_payload + "}";
        if (method.rfind("echo.", 0) == 0) return json_payload + "/" + method.substr(5);
        if (method == "boom") throw std::runtime_error("fixture endpoint failure");
        throw std::runtime_error("unknown method: " + method);
    }

    bool Subscribe(const std::string& method, const std::string& json_payload,
                   std::shared_ptr<plugins::PluginSubscriptionSink> sink) override {
        if (method != "ticks") return false;
        // Pushed from a thread the endpoint owns; the shared sink makes the
        // subscriber's disconnect safe whenever it lands.
        const int count = std::stoi(json_payload.empty() ? "1" : json_payload);
        std::thread([sink, count] {
            for (int i = 0; i < count; ++i) {
                if (!sink->Push("tick-" + std::to_string(i))) return;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }).detach();
        return true;
    }
};

FixtureEndpoint& Endpoint() {
    static FixtureEndpoint endpoint;
    return endpoint;
}

}  // namespace

extern "C" {
const char* minimal_plugin_abi_tag() { return minimal::common::AbiTag(); }
const char* minimal_plugin_name() { return "fixture-endpoint"; }
const char* minimal_plugin_version() { return "1.0.0"; }

void minimal_plugin_register_services(plugins::ServiceRegistry& registry,
                                      const plugins::PluginRegistrationContext&) {
    registry.Provide("fixture-endpoint", &Endpoint());
}
}
