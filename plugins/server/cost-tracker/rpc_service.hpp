#pragma once

// The PluginCall dispatch target, provided as "cost-tracker.rpc" (see
// minimal/plugins/plugin_rpc.hpp for the convention). This is the UI data
// path: a TUI client-plugin view, a web frontend, or a script fetches cost
// state through the generic unary PluginCall RPC and reads plugin-defined
// JSON. Core transports and never interprets it.
//
// Methods (JSON in, JSON out):
//   totals {"session_id": "..."} -> the session's totals PLUS its whole
//                                   subagent subtree's (parent_session_id
//                                   closure; the "includes_subagents" field
//                                   marks when children merged) and the
//                                   sessions' in-flight mid-turn rounds
//                                   (see in_flight.hpp) merged in, so a
//                                   status line can move DURING a turn --
//                                   subagent runs included. (see TotalsToJson)
//   list   {}                    -> {"sessions": [totals...]}   every session
//                                   (per-session, no roll-up)
//   refresh_rates {}             -> fetches both pricing snapshots now; same
//                                   path as the startup auto-refresh and the
//                                   server's --plugin-call flag.
//                                   Returns {"updated": N, "litellm_updated": N,
//                                   "openrouter_updated": N, "fetched_at":
//                                   "...", "warning": "..."} or !ok with the
//                                   failure (both sources down).
//
// Threading: Handle runs on gRPC handler threads, concurrently across calls;
// all state sits behind PluginStorage's per-call mutex, RateTable's own, and
// InFlightUsage's own.

#include <optional>
#include <string_view>

#include "cost_model.hpp"
#include "cost_settings.hpp"
#include "cost_store.hpp"
#include "in_flight.hpp"
#include "session_index.hpp"
#include "minimal/plugins/plugin_rpc.hpp"

namespace minimal::cost_tracker {

nlohmann::json TotalsAsJson(const SessionTotals& totals);
std::string TotalsToJson(const SessionTotals& totals);

class CostRpcService : public plugins::PluginRpcHandler {
public:
    // A null RateResolver renders costs as unknown rather than crashing --
    // the same degrade-not-crash rule as the store's null storage.
    CostRpcService(CostStore& store, RateTable& rates, const CostSettings& settings,
                   InFlightUsage& in_flight, SessionIndex& index)
        : store_(store), rates_(rates), settings_(settings), in_flight_(in_flight),
          index_(index) {}

    plugins::PluginRpcHandler::Reply Handle(const std::string& method,
                                            std::string_view json_payload) override;

    // The "totals" body for `session_id` and its subagent subtree, with the
    // "includes_subagents"/"in_flight" flags; nullopt when nothing is
    // recorded (stored or live). Shared by the "totals" method and the push
    // hub (totals_hub.hpp). Corrupt member blobs are skipped and logged.
    std::optional<nlohmann::json> SubtreeTotals(const std::string& session_id);

private:
    CostStore& store_;
    RateTable& rates_;
    const CostSettings& settings_;
    InFlightUsage& in_flight_;
    SessionIndex& index_;
};

}  // namespace minimal::cost_tracker
