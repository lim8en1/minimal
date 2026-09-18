#include "rates_refresh.hpp"

#include <chrono>

#include "minimal/common/logger.hpp"
#include "rates_fetch.hpp"

namespace minimal::cost_tracker {

bool RefreshRatesDue(const std::optional<FetchedRates>& litellm,
                     const std::optional<FetchedRates>& openrouter,
                     const CostSettings& settings) {
    if (settings.rates_refresh_days <= 0) return true;  // 0: fetch on every start
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    // Absent snapshot or a zero timestamp: no cache, or a cache written by a
    // build before fetched_at_unix existed -- both fetch.
    const auto stale = [now, horizon = settings.rates_refresh_days * 86400](
                           const std::optional<FetchedRates>& rates) {
        return !rates || rates->fetched_at_unix <= 0 || now - rates->fetched_at_unix >= horizon;
    };
    return stale(litellm) || stale(openrouter);
}

void RatesRefresher::MaybeStart() {
    // Storage is the fetch's cache; without the seam there is nothing to
    // refresh (bundled/config tiers still price every report).
    if (storage_ == nullptr) return;
    if (!RefreshRatesDue(store_.GetFetchedRates(), store_.GetOpenRouterFetchedRates(),
                         settings_)) {
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
        common::DefaultLogger().info("cost-tracker: refreshed {} model rate(s) at startup "
                                     "(LiteLLM {}, OpenRouter {}, snapshot of {}){}",
                                     outcome.updated, outcome.litellm_updated,
                                     outcome.openrouter_updated, outcome.fetched_at,
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
