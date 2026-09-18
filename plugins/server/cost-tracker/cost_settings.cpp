#include "cost_settings.hpp"

namespace minimal::cost_tracker {

CostSettings ParseCostSettings(const nlohmann::json& config) {
    CostSettings settings;
    if (!config.is_object()) return settings;
    if (const auto it = config.find("rates_fetch_url");
        it != config.end() && it->is_string() && !it->get<std::string>().empty()) {
        settings.rates_fetch_url = it->get<std::string>();
    }
    if (const auto it = config.find("openrouter_rates_fetch_url");
        it != config.end() && it->is_string() && !it->get<std::string>().empty()) {
        settings.openrouter_rates_fetch_url = it->get<std::string>();
    }
    if (const auto it = config.find("rates_fetch_timeout_ms");
        it != config.end() && it->is_number_integer() && it->get<int>() > 0) {
        settings.rates_fetch_timeout_ms = it->get<int>();
    }
    // 0 is meaningful ("always refresh"), so it is the one accepted
    // non-positive value; anything negative or non-integer keeps the default.
    if (const auto it = config.find("rates_refresh_days");
        it != config.end() && it->is_number_integer() && it->get<int>() >= 0) {
        settings.rates_refresh_days = it->get<int>();
    }
    return settings;
}

}  // namespace minimal::cost_tracker
