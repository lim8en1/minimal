#pragma once

// The generic client channel's endpoint, provided as "cost-tracker.endpoint"
// (plugin_abi.h's PluginEndpoint convention). The server's PluginCall
// dispatch prefers ".endpoint" over ".rpc", so once this is Provided every
// client PluginCall lands here:
//   - Call(method, payload) is the ".rpc" service verbatim (rpc_service.hpp).
//     A plugin-domain refusal throws, which the transport answers INTERNAL --
//     the caller sees the same failed call ".rpc" gave it.
//   - Subscribe("totals", _) attaches the stream to the push hub
//     (totals_hub.hpp); anything else is refused (NOT_FOUND).
// ".rpc" stays Provided too: the server's --refresh-cost-rates flag resolves
// it directly (src/server/main.cpp).

#include <memory>
#include <string>

#include "minimal/plugins/plugin_abi.h"
#include "rpc_service.hpp"
#include "totals_hub.hpp"

namespace minimal::cost_tracker {

class CostEndpoint final : public plugins::PluginEndpoint {
public:
    // Borrowed; process lifetime.
    CostEndpoint(CostRpcService& rpc, TotalsHub& hub) : rpc_(rpc), hub_(hub) {}

    std::string Call(const std::string& method, const std::string& json_payload) override;

    bool Subscribe(const std::string& method, const std::string& json_payload,
                   std::shared_ptr<plugins::PluginSubscriptionSink> sink) override;

private:
    CostRpcService& rpc_;
    TotalsHub& hub_;
};

}  // namespace minimal::cost_tracker
