#pragma once

// The startup auto-refresh: a server start whose cached pricing snapshot is
// older than CostSettings::rates_refresh_days fetches a fresh one on ONE
// bounded background thread, kicked from registration. No core seam runs a
// plugin after model discovery (every registration hook completes before the
// server listens), so "rates fresh by the first turn" is delivered by racing
// the warm-up rather than sequencing after it -- the fetch is bounded by
// CostSettings::rates_fetch_timeout_ms and never blocks boot, a turn, or the
// listen banner.
//
// This is where the plugin's old "no network at startup" stance (it was a
// command, /cost-update-rates, until the auto-refresh replaced it) is
// deliberately reversed: pricing that silently ages out was costing every
// session after day ~7 an unpriced turn, and the fetch's own degrade rules --
// stale price on failure, per-source independence, bundled/config tiers
// unaffected -- carry over unchanged. Skipped entirely when the host does not
// provide plugin storage: FetchAndUpdateRates needs somewhere to cache.
//
// LIFETIME. The worker holds this object's references, so Shutdown() must run
// before the PluginStorage the fetch writes to dies. Registration gives a
// plugin no teardown hook, so the join lives in RatesRefresherOwner (below):
// an all-default SessionObserver whose destructor is the hook, registered so
// the observer registry's own teardown -- which drains first and is destroyed
// before plugin_storage in main()'s reverse-order cascade -- guarantees the
// ordering. Same discipline as session-titler's drain-on-destroy workers.
//
// THREADING. MaybeStart is main-thread (registration); the worker touches
// only CostStore/RateTable, both internally synchronized; the .rpc service's
// refresh_rates may run concurrently and serializes on the same mutexes --
// last whole-snapshot write wins, no torn state. RefreshRatesDue is a pure
// function so the gate is testable without a thread.

#include <atomic>
#include <optional>
#include <thread>
#include <vector>

#include "cost_model.hpp"
#include "cost_settings.hpp"
#include "cost_store.hpp"
#include "minimal/agent/session_observer.hpp"
#include "minimal/plugins/plugin_storage.hpp"

namespace minimal::cost_tracker {

// True when a server start should fetch, given one cached snapshot per
// configured source: no sources never fetches; rates_refresh_days == 0
// always fetches; otherwise any of -- no cached snapshot at all, a snapshot
// with no machine timestamp (pre-dates the field: assume old), or a snapshot
// past the gate. EVERY snapshot must be fresh for a skip: one stale tier
// means one tier of prices silently aging out, which is exactly what the gate
// exists to prevent. Errs toward fetching, never toward keeping a stale price.
bool RefreshRatesDue(const std::vector<std::optional<FetchedRates>>& snapshots,
                     const CostSettings& settings);

class RatesRefresher {
public:
    // All borrowed; storage may be null (then MaybeStart never starts).
    RatesRefresher(const CostSettings& settings, CostStore& store, RateTable& rates,
                   plugins::PluginStorage* storage)
        : settings_(settings), store_(store), rates_(rates), storage_(storage) {}

    RatesRefresher(const RatesRefresher&) = delete;
    RatesRefresher& operator=(const RatesRefresher&) = delete;

    // Starts the worker iff the cached snapshot is stale and storage exists.
    // Idempotent: the shared state is per-process and every registration hook
    // funnels through the same EnsureSharedState, but this costs one atomic.
    // Never blocks: the fetch itself runs on the worker.
    void MaybeStart();

    // Joins the worker (a no-op when never started). Called at teardown from
    // RatesRefresherOwner's destructor -- never from a static's destructor,
    // which would run after plugin_storage is gone.
    void Shutdown();

private:
    void Run();

    const CostSettings& settings_;
    CostStore& store_;
    RateTable& rates_;
    plugins::PluginStorage* storage_;
    std::atomic<bool> started_{false};
    std::thread worker_;
};

// The teardown hook the plugin ABI does not otherwise give a plugin: an
// observer with no callbacks, registered only so its destructor runs at the
// right point of main()'s destruction cascade (after the observer registry
// has drained, before plugin_storage is destroyed). Holds nothing.
class RatesRefresherOwner : public agent::SessionObserver {
public:
    explicit RatesRefresherOwner(RatesRefresher& refresher) : refresher_(refresher) {}
    ~RatesRefresherOwner() override { refresher_.Shutdown(); }

private:
    RatesRefresher& refresher_;
};

}  // namespace minimal::cost_tracker
