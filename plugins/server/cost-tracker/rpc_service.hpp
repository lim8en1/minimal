#pragma once

// The PluginCall dispatch target, provided as "cost-tracker.rpc" (see
// minimal/plugins/plugin_rpc.hpp for the convention). This is the UI data
// path: a TUI client-plugin view, a web frontend, or a script fetches cost
// state through the generic unary PluginCall RPC and reads plugin-defined
// JSON. Core transports and never interprets it.
//
// Methods (JSON in, JSON out):
//   totals {"session_id": "..."} -> the session's totals (see TotalsToJson)
//   list   {}                    -> {"sessions": [totals...]}   every session
//   refresh_rates {}             -> fetches the LiteLLM snapshot now; same
//                                   path as /cost-update-rates. Returns
//                                   {"updated": N, "fetched_at": "..."} or
//                                   !ok with the failure.
//
// Threading: Handle runs on gRPC handler threads, concurrently across calls;
// all state sits behind PluginStorage's per-call mutex and RateTable's own.

#include <string_view>

#include "cost_model.hpp"
#include "cost_settings.hpp"
#include "cost_store.hpp"
#include "minimal/plugins/plugin_rpc.hpp"

namespace minimal::cost_tracker {

nlohmann::json TotalsAsJson(const SessionTotals& totals);
std::string TotalsToJson(const SessionTotals& totals);

class CostRpcService : public plugins::PluginRpcHandler {
public:
    // A null RateResolver renders costs as unknown rather than crashing --
    // the same degrade-not-crash rule as the store's null storage.
    CostRpcService(CostStore& store, RateTable& rates, const CostSettings& settings)
        : store_(store), rates_(rates), settings_(settings) {}

    plugins::PluginRpcHandler::Reply Handle(const std::string& method,
                                            std::string_view json_payload) override;

private:
    CostStore& store_;
    RateTable& rates_;
    const CostSettings& settings_;
};

}  // namespace minimal::cost_tracker
