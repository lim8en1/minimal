#pragma once

// cost-tracker policy: every value here is a judgment call, not a fact or a
// derivation, and states why it is what it is. See ARCHITECTURE.md
// "Policy constants".

#include <chrono>

namespace minimal::cost_tracker {

// Rate tables (LiteLLM, OpenRouter) are fetched with this budget (a few MB
// over the internet) and refreshed when older than this many days: prices
// change by the month, not the hour.
inline constexpr std::chrono::milliseconds kDefaultRatesFetchTimeout = std::chrono::seconds(30);
inline constexpr int kDefaultRatesRefreshDays = 7;

// Totals pushes coalesce over this window: a turn's usage events arrive in a
// burst, and the status segment needs one update for it.
inline constexpr std::chrono::milliseconds kPushDebounce{200};

}  // namespace minimal::cost_tracker
