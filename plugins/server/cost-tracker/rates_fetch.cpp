#include "rates_fetch.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>
#include <thread>
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

// Unix seconds for the same kind of instant -- the snapshot's machine-usable
// timestamp, the one the startup staleness gate reads (the ISO string above is
// diagnostic only).
std::int64_t UnixSecondsNow() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

RatesFetchOutcome FetchAndUpdateRates(const CostSettings& settings, CostStore& store,
                                      RateTable& rates) {
    RatesFetchOutcome outcome;

    // Fetched CONCURRENTLY, not sequentially: each is an independent
    // blocking HTTP GET (own socket, own net::HttpClient instance -- no
    // shared state between the two calls), so running them one after the
    // other doubled worst-case latency for no reason. This matters most on
    // the paths that block a human or the whole process on the result:
    // startup auto-refresh, `--refresh-cost-rates`, and the `.rpc
    // refresh_rates` handler thread all run through here.
    std::string litellm_error;
    std::string openrouter_error;
    std::map<std::string, Rate> litellm;
    std::map<std::string, Rate> openrouter;
    std::thread litellm_thread([&] {
        litellm = FetchSource(settings.rates_fetch_url, settings.rates_fetch_timeout_ms, "LiteLLM",
                              IngestLiteLlmPrices, litellm_error);
    });
    openrouter = FetchSource(settings.openrouter_rates_fetch_url, settings.rates_fetch_timeout_ms,
                             "OpenRouter", IngestOpenRouterPrices, openrouter_error);
    litellm_thread.join();

    if (litellm.empty() && openrouter.empty()) {
        outcome.error = litellm_error + "; " + openrouter_error;
        return outcome;
    }

    outcome.fetched_at = Iso8601UtcNow();
    const std::int64_t fetched_at_unix = UnixSecondsNow();
    // Each cache write is its own try/catch (not one try wrapping both): a
    // single shared try meant a LiteLLM write that succeeded was silently
    // discarded from the caller's point of view whenever the SUBSEQUENT
    // OpenRouter write threw -- `RefreshFetched()` sat after the catch and
    // never ran, so the live RateTable kept stale prices even though storage
    // already had the fresh LiteLLM snapshot. Whichever snapshot persists
    // should take effect regardless of what happens to the other.
    std::string litellm_store_error;
    if (!litellm.empty()) {
        try {
            FetchedRates fetched;
            fetched.fetched_at = outcome.fetched_at;
            fetched.fetched_at_unix = fetched_at_unix;
            const int count = static_cast<int>(litellm.size());
            fetched.models = std::move(litellm);
            store.SetFetchedRates(fetched);
            outcome.litellm_updated = count;
        } catch (const std::exception& err) {
            litellm_store_error = std::string("caching the LiteLLM rates failed: ") + err.what();
        }
    }
    std::string openrouter_store_error;
    if (!openrouter.empty()) {
        try {
            FetchedRates fetched;
            fetched.fetched_at = outcome.fetched_at;
            fetched.fetched_at_unix = fetched_at_unix;
            const int count = static_cast<int>(openrouter.size());
            fetched.models = std::move(openrouter);
            store.SetOpenRouterFetchedRates(fetched);
            outcome.openrouter_updated = count;
        } catch (const std::exception& err) {
            openrouter_store_error =
                std::string("caching the OpenRouter rates failed: ") + err.what();
        }
    }

    const bool any_stored = outcome.litellm_updated > 0 || outcome.openrouter_updated > 0;
    if (any_stored) rates.RefreshFetched();

    outcome.ok = any_stored;
    outcome.updated = outcome.litellm_updated + outcome.openrouter_updated;
    // A degraded success: the fresh tier takes effect while the other stays
    // stale -- the same rule a total failure obeys, just per source. Fetch
    // errors and store errors are both reported here, whichever occurred.
    std::string notes;
    for (const std::string* note :
        {&litellm_error, &openrouter_error, &litellm_store_error, &openrouter_store_error}) {
        if (note->empty()) continue;
        notes += notes.empty() ? *note : "; " + *note;
    }
    if (!any_stored) {
        outcome.error = notes;
    } else if (!notes.empty()) {
        outcome.warning = notes;
    }
    return outcome;
}

}  // namespace minimal::cost_tracker
