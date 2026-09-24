// cost-tracker -- per-session token usage and estimated cost.
//
// Five seams, one owner, one store:
//   - the SessionObserver records every completed turn (the store's only
//     writer) and feeds every provider round into the in-flight provisional
//     total, so a live UI can show usage MID-turn -- subagent runs included
//     -- without waiting for the turn to end;
//   - /cost renders a session's (or every session's) totals as text;
//   - "cost-tracker.tracking" exposes read-only totals to other server-side
//     plugins through the §2.2 service channel;
//   - "cost-tracker.rpc" serves the same state as JSON over the generic
//     PluginCall channel -- the data path the future UI view consumes -- and
//     its refresh_rates method is what the server's --refresh-cost-rates
//     flag drives;
//   - the startup auto-refresh (rates_refresh.hpp) fetches the LiteLLM and
//     OpenRouter pricing snapshots when the cached ones are older than
//     CostSettings::rates_refresh_days -- the opt-in network rule's
//     replacement, bounded and background, with the join parked on the
//     RatesRefresherOwner observer.
// All of them resolve prices through the same RateTable, so a report can never
// disagree with itself, and the command-free sibling of the checklist shape:
// the model has no tool here because cost is the operator's business.
//
// COST IS DERIVED, NOT REQUESTED. Nothing in the agent loop or the provider
// changes; the observer is the sanctioned push channel for exactly this
// (ARCHITECTURE.md names usage tracking among SessionObserver's uses, and
// TurnSummary::model exists so a per-model rate is resolvable at all).
//
// PRICING. Four sources, config > openrouter-fetched > litellm-fetched >
// bundled: the user's "rates" slice in plugins.json, the OpenRouter snapshot
// (its slugs ARE the wire model ids an OpenRouter-shaped provider sends, so
// no provider naming is consulted), the LiteLLM snapshot (both cached by the
// startup auto-refresh, the --refresh-cost-rates flag, or the .rpc service's
// refresh_rates), and the seeded cost-rates.json in the config dir
// (bundled/ treatment: user-editable text, never overwritten, never compiled
// in). Keys are model ids -- the ccusage convention. A model with no rate
// anywhere, and a turn whose usage the backend reported as all zeros (the
// known proxy gap), render as "unknown", never as $0.00.
//
// LIFETIME. History blobs survive session termination and are NOT purged on
// OnSessionEnded -- the checklist precedent: a resumed session keeps its
// history, and there is no session-deleted signal to hook.

#include "minimal/plugins/plugin_abi.h"

#include <fstream>
#include <memory>

#include "cost_command.hpp"
#include "cost_observer.hpp"
#include "cost_settings.hpp"
#include "cost_store.hpp"
#include "cost_tracking_service.hpp"
#include "cost_tracking_service_impl.hpp"
#include "endpoint.hpp"
#include "in_flight.hpp"
#include "rates_refresh.hpp"
#include "rpc_service.hpp"
#include "session_index.hpp"
#include "totals_hub.hpp"

namespace {

constexpr char kBundledRatesFile[] = "cost-rates.json";

// One set of process-lifetime statics, constructed at whichever hook runs
// first -- the checklist pattern. The store's storage pointer and the rate
// sources are fixed at registration; nothing else mutates them.
std::shared_ptr<minimal::cost_tracker::CostStore>& SharedStore() {
    static std::shared_ptr<minimal::cost_tracker::CostStore> store;
    return store;
}

std::shared_ptr<minimal::cost_tracker::InFlightUsage>& SharedInFlight() {
    static std::shared_ptr<minimal::cost_tracker::InFlightUsage> in_flight;
    return in_flight;
}

std::shared_ptr<minimal::cost_tracker::RateTable>& SharedRates() {
    static std::shared_ptr<minimal::cost_tracker::RateTable> rates;
    return rates;
}

std::shared_ptr<minimal::cost_tracker::CostSettings>& SharedSettings() {
    static std::shared_ptr<minimal::cost_tracker::CostSettings> settings;
    return settings;
}

std::shared_ptr<minimal::cost_tracker::SessionIndex>& SharedIndex() {
    static std::shared_ptr<minimal::cost_tracker::SessionIndex> index;
    return index;
}

std::shared_ptr<minimal::cost_tracker::CostRpcService>& SharedRpc() {
    static std::shared_ptr<minimal::cost_tracker::CostRpcService> rpc;
    return rpc;
}

// The totals push hub. Same teardown discipline as the refresher below: its
// worker reads the store, so Shutdown() runs from TotalsHubOwner's
// destructor, never from a static destructor after plugin_storage is gone.
std::shared_ptr<minimal::cost_tracker::TotalsHub>& SharedHub() {
    static std::shared_ptr<minimal::cost_tracker::TotalsHub> hub;
    return hub;
}

// The startup auto-refresh. Not destroyed with the process: Shutdown() runs
// from RatesRefresherOwner's destructor (see below), while the worker can
// still see a living PluginStorage; a static destructor would join after
// plugin_storage is gone.
std::shared_ptr<minimal::cost_tracker::RatesRefresher>& SharedRefresher() {
    static std::shared_ptr<minimal::cost_tracker::RatesRefresher> refresher;
    return refresher;
}

// The bundled snapshot, read from the config dir -- the seeded file the user
// may edit, never a copy compiled into the .so. A missing or malformed file
// is a missing tier, not an error: pricing then comes from the config slice
// or a fetch, and unpriced models render as "unknown".
nlohmann::json LoadBundledRates() {
    const auto path = minimal::common::ConfigDir() / kBundledRatesFile;
    std::ifstream in(path);
    if (!in) return nlohmann::json::object();
    try {
        nlohmann::json document = nlohmann::json::parse(in);
        if (document.is_object()) {
            if (const auto it = document.find("models");
                it != document.end() && it->is_object()) {
                return *it;
            }
        }
    } catch (const std::exception&) {
        // Degrade below.
    }
    return nlohmann::json::object();
}

void EnsureSharedState(const minimal::plugins::PluginRegistrationContext& context) {
    if (!SharedSettings()) SharedSettings() = std::make_shared<minimal::cost_tracker::CostSettings>(
        minimal::cost_tracker::ParseCostSettings(context.config));
    if (!SharedRates()) {
        minimal::cost_tracker::RateSources sources;
        sources.bundled_models = LoadBundledRates();
        if (const auto it = context.config.find("rates"); it != context.config.end()) {
            sources.config_rates = *it;
        }
        SharedRates() = std::make_shared<minimal::cost_tracker::RateTable>(
            std::move(sources), context.storage);
        // Pick up a snapshot fetched by an earlier process run.
        SharedRates()->RefreshFetched();
    }
    if (!SharedStore()) {
        SharedStore() = std::make_shared<minimal::cost_tracker::CostStore>(context.storage);
    }
    if (!SharedInFlight()) SharedInFlight() = std::make_shared<minimal::cost_tracker::InFlightUsage>();
    if (!SharedIndex()) {
        SharedIndex() = std::make_shared<minimal::cost_tracker::SessionIndex>(*SharedStore());
    }
    if (!SharedRpc()) {
        SharedRpc() = std::make_shared<minimal::cost_tracker::CostRpcService>(
            *SharedStore(), *SharedRates(), *SharedSettings(), *SharedInFlight(), *SharedIndex());
    }
    if (!SharedHub()) {
        SharedHub() = std::make_shared<minimal::cost_tracker::TotalsHub>(
            *SharedIndex(), [rpc = SharedRpc()](const std::string& session_id) {
                return rpc->SubtreeTotals(session_id);
            });
    }
    if (!SharedRefresher()) {
        SharedRefresher() = std::make_shared<minimal::cost_tracker::RatesRefresher>(
            *SharedSettings(), *SharedStore(), *SharedRates(), context.storage);
    }
}

}  // namespace

extern "C" {

MINIMAL_PLUGIN_ABI_SYMBOLS(minimal::cost_tracker::kPluginName)

void minimal_plugin_register_commands(
    minimal::commands::CommandRegistry& registry,
    const minimal::plugins::PluginRegistrationContext& context) {
    EnsureSharedState(context);
    registry.Register(std::make_unique<minimal::cost_tracker::CostCommand>(
        *SharedStore(), *SharedRates()));
}

void minimal_plugin_register_session_observer(
    minimal::agent::SessionObserverRegistry& registry,
    const minimal::plugins::PluginRegistrationContext& context) {
    EnsureSharedState(context);
    // The auto-refresh fires here, where the shared state is first guaranteed
    // to exist; the same registration plants the observer whose destructor
    // will join the worker at teardown.
    SharedRefresher()->MaybeStart();
    registry.Register(std::make_shared<minimal::cost_tracker::CostSessionObserver>(
        *SharedStore(), *SharedInFlight(), SharedIndex().get(), SharedHub().get()));
    registry.Register(std::make_shared<minimal::cost_tracker::RatesRefresherOwner>(
        *SharedRefresher()));
    registry.Register(std::make_shared<minimal::cost_tracker::TotalsHubOwner>(*SharedHub()));
}

void minimal_plugin_register_services(
    minimal::plugins::ServiceRegistry& registry,
    const minimal::plugins::PluginRegistrationContext& context) {
    EnsureSharedState(context);

    static minimal::cost_tracker::CostTrackingServiceImpl tracking(*SharedStore(),
                                                                   *SharedRates());
    registry.Provide(minimal::cost_tracker::kCostTrackingServiceName, &tracking);

    // The PluginCall dispatch target, under the "<plugin_name>.rpc" convention
    // (see minimal/plugins/plugin_rpc.hpp). The server's --refresh-cost-rates
    // flag resolves this one directly.
    registry.Provide(minimal::plugins::PluginRpcServiceName(minimal::cost_tracker::kPluginName),
                     SharedRpc().get());

    // The UI data path: PluginCall (same methods as .rpc, which the dispatch
    // now reaches through here) plus the PluginSubscribe "totals" push stream.
    static minimal::cost_tracker::CostEndpoint endpoint(*SharedRpc(), *SharedHub());
    registry.Provide(
        minimal::plugins::PluginEndpointServiceName(minimal::cost_tracker::kPluginName), &endpoint);
}

}  // extern "C"
