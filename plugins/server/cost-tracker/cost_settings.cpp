#include "cost_settings.hpp"
#include <algorithm>

#include <limits>

#include "minimal/common/config_reader.hpp"
#include "minimal/common/clock.hpp"

namespace minimal::cost_tracker {

CostSettings ParseCostSettings(const nlohmann::json& config) {
    CostSettings settings;
    config::Reader reader(config, "plugins.json", "cost-tracker.config");
    settings.rates_fetch_url =
        reader.String("rates_fetch_url", settings.rates_fetch_url, /*allow_empty=*/false);
    settings.openrouter_rates_fetch_url = reader.String(
        "openrouter_rates_fetch_url", settings.openrouter_rates_fetch_url, /*allow_empty=*/false);
    settings.rates_fetch_timeout_ms = static_cast<int>(reader.Int(
        "rates_fetch_timeout_ms", settings.rates_fetch_timeout_ms, 1,
        std::min<std::int64_t>(std::numeric_limits<int>::max(), common::kMaxDeadline.count())));
    // 0 is meaningful ("always refresh").
    settings.rates_refresh_days = static_cast<int>(
        reader.Int("rates_refresh_days", settings.rates_refresh_days, 0, std::numeric_limits<int>::max()));
    if (auto rates = reader.MaybeObject("rates")) settings.rates = rates->node();
    reader.Finish();
    return settings;
}

}  // namespace minimal::cost_tracker
