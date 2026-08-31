#include "rates_fetch.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#include "minimal/llm/http_client.hpp"

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

}  // namespace

RatesFetchOutcome FetchAndUpdateRates(const CostSettings& settings, CostStore& store,
                                      RateTable& rates) {
    RatesFetchOutcome outcome;
    llm::HttpRequest request;
    request.url = settings.rates_fetch_url;
    request.request_timeout = std::chrono::milliseconds(settings.rates_fetch_timeout_ms);
    llm::HttpResponse response;
    try {
        response = llm::HttpClient{}.Get(request);
    } catch (const llm::HttpError& error) {
        outcome.error = std::string("fetching rates failed: ") + error.what();
        return outcome;
    }
    if (response.status < 200 || response.status >= 300) {
        outcome.error = "fetching rates failed: HTTP " + std::to_string(response.status);
        return outcome;
    }

    nlohmann::json document;
    try {
        document = nlohmann::json::parse(response.body);
    } catch (const std::exception& error) {
        outcome.error = std::string("the rates snapshot is not valid JSON: ") + error.what();
        return outcome;
    }
    const auto models = IngestLiteLlmPrices(document);
    if (models.empty()) {
        outcome.error = "the rates snapshot contained no per-token-priced models";
        return outcome;
    }

    FetchedRates fetched;
    fetched.fetched_at = Iso8601UtcNow();
    fetched.models = models;
    try {
        store.SetFetchedRates(fetched);
    } catch (const std::exception& error) {
        outcome.error = std::string("caching the fetched rates failed: ") + error.what();
        return outcome;
    }
    rates.RefreshFetched();

    outcome.ok = true;
    outcome.updated = static_cast<int>(fetched.models.size());
    outcome.fetched_at = fetched.fetched_at;
    return outcome;
}

}  // namespace minimal::cost_tracker
