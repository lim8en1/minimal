#include "rates_fetch.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>

#include "minimal/net/http_client.hpp"

namespace minimal::cost_tracker {
namespace {

std::string Iso8601UtcNow() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    gmtime_r(&time, &utc);
    std::ostringstream out;
    out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

// One source: HTTP GET, parse, ingest. On ANY failure returns an empty map
// and fills `error` -- never throws, never touches storage.
std::map<std::string, Rate> FetchSource(
    const std::string& url, int timeout_ms, const std::string& label,
    std::map<std::string, Rate> (*ingest)(const nlohmann::json&), std::string& error) {
    net::HttpRequest request;
    request.url = url;
    request.request_timeout = std::chrono::milliseconds(timeout_ms);
    net::HttpResponse response;
    try {
        response = net::HttpClient{}.Get(request);
    } catch (const net::HttpError& err) {
        error = label + " fetch failed: " + err.what();
        return {};
    }
    if (response.status < 200 || response.status >= 300) {
        error = label + " fetch failed: HTTP " + std::to_string(response.status);
        return {};
    }
    nlohmann::json document;
    try {
        document = nlohmann::json::parse(response.body);
    } catch (const std::exception& err) {
        error = label + " snapshot is not valid JSON: " + err.what();
        return {};
    }
    auto rates = ingest(document);
    if (rates.empty()) {
        error = "the " + label + " snapshot contained no per-token-priced models";
    }
    return rates;
}

}  // namespace

RatesFetchOutcome FetchAndUpdateRates(const CostSettings& settings, CostStore& store,
                                      RateTable& rates) {
    RatesFetchOutcome outcome;

    std::string litellm_error;
    std::string openrouter_error;
    std::map<std::string, Rate> litellm =
        FetchSource(settings.rates_fetch_url, settings.rates_fetch_timeout_ms, "LiteLLM",
                    IngestLiteLlmPrices, litellm_error);
    std::map<std::string, Rate> openrouter =
        FetchSource(settings.openrouter_rates_fetch_url, settings.rates_fetch_timeout_ms,
                    "OpenRouter", IngestOpenRouterPrices, openrouter_error);

    if (litellm.empty() && openrouter.empty()) {
        outcome.error = litellm_error + "; " + openrouter_error;
        return outcome;
    }

    outcome.fetched_at = Iso8601UtcNow();
    try {
        if (!litellm.empty()) {
            FetchedRates fetched;
            fetched.fetched_at = outcome.fetched_at;
            fetched.models = std::move(litellm);
            outcome.litellm_updated = static_cast<int>(fetched.models.size());
            store.SetFetchedRates(fetched);
        }
        if (!openrouter.empty()) {
            FetchedRates fetched;
            fetched.fetched_at = outcome.fetched_at;
            fetched.models = std::move(openrouter);
            outcome.openrouter_updated = static_cast<int>(fetched.models.size());
            store.SetOpenRouterFetchedRates(fetched);
        }
    } catch (const std::exception& err) {
        outcome.error = std::string("caching the fetched rates failed: ") + err.what();
        return outcome;
    }
    rates.RefreshFetched();

    outcome.ok = true;
    outcome.updated = outcome.litellm_updated + outcome.openrouter_updated;
    // A degraded success: the fresh tier takes effect while the other stays
    // stale -- the same rule a total failure obeys, just per source.
    if (!litellm_error.empty()) outcome.warning = litellm_error;
    if (!openrouter_error.empty()) {
        outcome.warning =
            outcome.warning.empty() ? openrouter_error : outcome.warning + "; " + openrouter_error;
    }
    return outcome;
}

}  // namespace minimal::cost_tracker
