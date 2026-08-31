#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <string>

#include "minimal/plugins/plugin_rpc.hpp"

using namespace minimal;
using plugins::PluginRpcHandler;
using plugins::PluginRpcServiceName;
using plugins::RpcOutcome;

namespace {

class EchoHandler final : public plugins::PluginRpcHandler {
public:
    Reply Handle(const std::string& method, std::string_view payload) override {
        if (method == "boom") throw std::runtime_error("handler exploded");
        if (method == "refuse") return Reply{false, "", "malformed request"};
        return Reply{true, "echo:" + std::string(payload), ""};
    }
};

struct Registry {
    plugins::ServiceRegistry services;
    EchoHandler handler;

    Registry() { services.Provide(PluginRpcServiceName("echo"), &handler); }
};

}  // namespace

TEST_CASE("a registered handler is found and its reply returned", "[plugin_rpc]") {
    Registry r;
    const auto result = plugins::DispatchPluginCall(&r.services, "echo", "ping", "hello");
    CHECK(result.outcome == RpcOutcome::kOk);
    CHECK(result.payload == "echo:hello");
    CHECK(result.error.empty());
}

TEST_CASE("an unknown plugin is NOT_FOUND, not an error", "[plugin_rpc]") {
    Registry r;
    // Disabled plugins are a runtime condition: the contract is a clean
    // classified answer, never a throw.
    const auto result = plugins::DispatchPluginCall(&r.services, "ghost", "ping", "");
    CHECK(result.outcome == RpcOutcome::kNotFound);
    CHECK(result.error.find("ghost") != std::string::npos);
}

TEST_CASE("a plugin without an .rpc service is NOT_FOUND too", "[plugin_rpc]") {
    plugins::ServiceRegistry services;
    // Plain §2.2 service under the bare plugin name: the dispatch convention
    // deliberately does not collide with it.
    int plain_service = 0;
    services.Provide("quiet", &plain_service);

    const auto result = plugins::DispatchPluginCall(&services, "quiet", "ping", "");
    CHECK(result.outcome == RpcOutcome::kNotFound);
}

TEST_CASE("empty plugin_name or method is INVALID before any lookup", "[plugin_rpc]") {
    Registry r;
    CHECK(plugins::DispatchPluginCall(&r.services, "", "ping", "").outcome ==
          RpcOutcome::kInvalid);
    CHECK(plugins::DispatchPluginCall(&r.services, "echo", "", "").outcome ==
          RpcOutcome::kInvalid);
}

TEST_CASE("a handler-reported failure stays distinct from a crash", "[plugin_rpc]") {
    Registry r;
    const auto refused = plugins::DispatchPluginCall(&r.services, "echo", "refuse", "");
    CHECK(refused.outcome == RpcOutcome::kInvalid);
    CHECK(refused.error == "malformed request");

    const auto exploded = plugins::DispatchPluginCall(&r.services, "echo", "boom", "");
    CHECK(exploded.outcome == RpcOutcome::kInternal);
    CHECK(exploded.error.find("handler exploded") != std::string::npos);
}

TEST_CASE("a null registry is NOT_FOUND, and a throwing non-standard exception is contained",
          "[plugin_rpc]") {
    CHECK(plugins::DispatchPluginCall(nullptr, "echo", "ping", "").outcome ==
          RpcOutcome::kNotFound);
}
