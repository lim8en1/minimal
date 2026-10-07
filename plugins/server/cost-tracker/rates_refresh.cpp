#include "rates_refresh.hpp"

#include <algorithm>
#include <chrono>
#include <string>

#include "minimal/common/logger.hpp"
#include "rates_fetch.hpp"

namespace minimal::cost_tracker {

bool RefreshRatesDue(const std::vector<std::optional<FetchedRates>>& snapshots,
                     const CostSettings& settings) {
    if (snapshots.empty()) return false;  // no sources: nothing to fetch
    if (settings.rates_refresh_days <= 0) return true;  // 0: fetch on every start
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    // Absent snapshot or a zero timestamp: no cache, or a cache written by a
    // build before fetched_at_unix existed -- both fetch.
    const auto stale = [now, horizon = std::chrono::seconds(std::chrono::days(settings.rates_refresh_days)).count()](
                           const std::optional<FetchedRates>& rates) {
        return !rates || rates->fetched_at_unix <= 0 || now - rates->fetched_at_unix >= horizon;
    };
    return std::any_of(snapshots.begin(), snapshots.end(), stale);
}

void RatesRefresher::MaybeStart() {
    // Storage is the fetch's cache; without the seam there is nothing to
    // refresh (bundled/config tiers still price every report).
    if (storage_ == nullptr) return;
    // A source removed or renamed since the last start: its snapshot prices
    // nothing any more (RateTable reads only configured names), so it goes.
    std::vector<std::string> configured;
    for (const auto& source : settings_.rate_sources) configured.push_back(source.name);
    try {
        if (const auto removed = store_.PruneFetchedRates(configured); !removed.empty()) {
            std::string names;
            for (const auto& name : removed) names += (names.empty() ? "" : ", ") + name;
            common::DefaultLogger().info(
                "cost-tracker: dropped the cached prices of {} -- no longer in rate_sources", names);
        }
    } catch (const std::exception& error) {
        common::DefaultLogger().warn("cost-tracker: pruning cached prices failed: {}", error.what());
    }
    std::vector<std::optional<FetchedRates>> snapshots;
    for (const auto& source : settings_.rate_sources) {
        snapshots.push_back(store_.GetFetchedRates(source.name));
    }
    if (!RefreshRatesDue(snapshots, settings_)) {
        // Said, not silent: a skipped refresh must not read like one that
        // never ran. The oldest snapshot is what the next fetch waits on.
        const auto oldest = std::min_element(
            snapshots.begin(), snapshots.end(), [](const auto& a, const auto& b) {
                return a && b && a->fetched_at_unix < b->fetched_at_unix;
            });
        if (oldest != snapshots.end() && *oldest) {
            common::DefaultLogger().info(
                "cost-tracker: pricing snapshots are fresh (oldest fetched {}, refreshed every "
                "{} day(s)); not refreshing at startup",
                (*oldest)->fetched_at, settings_.rates_refresh_days);
        }
        return;
    }
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true)) return;
    worker_ = std::thread([this] { Run(); });
}

void RatesRefresher::Run() {
    // The fetch carries its own degrade rules: per-source independence, the
    // previous snapshot staying in force on failure. A failed refresh is a
    // log line, never an error surfaced anywhere -- the auto path has no
    // caller to answer.
    const auto outcome = FetchAndUpdateRates(settings_, store_, rates_);
    if (outcome.ok) {
        std::string per_source;
        for (const auto& source : outcome.sources) {
            per_source += (per_source.empty() ? "" : ", ") + source.name + " " +
                          std::to_string(source.updated);
        }
        common::DefaultLogger().info("cost-tracker: refreshed {} model rate(s) at startup "
                                     "({}, snapshot of {}){}",
                                     outcome.updated, per_source, outcome.fetched_at,
                                     outcome.warning.empty() ? "" : "; " + outcome.warning);
    } else {
        common::DefaultLogger().warn("cost-tracker: startup rates refresh failed; keeping the "
                                     "previous prices: {}",
                                     outcome.error);
    }
}

void RatesRefresher::Shutdown() {
    if (worker_.joinable()) worker_.join();
}

}  // namespace minimal::cost_tracker
