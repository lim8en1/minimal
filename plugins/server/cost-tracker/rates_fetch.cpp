#include "rates_fetch.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>

#include <nlohmann/json.hpp>

#include "minimal/net/http_client.hpp"
#include "minimal/common/http_status.hpp"

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

// One source: HTTP GET (with its bearer, when configured), parse, ingest. On
// ANY failure returns an empty map and fills `error` -- never throws, never
// touches storage.
std::map<std::string, Rate> FetchSource(const RateSourceSettings& source, int timeout_ms,
                                        std::string& error) {
    const std::string& label = source.name;
    net::HttpRequest request;
    request.url = source.url;
    request.request_timeout = std::chrono::milliseconds(timeout_ms);
    if (source.api_key.configured()) {
        try {
            request.headers.push_back({"Authorization", "Bearer " + source.api_key.Get().value});
        } catch (const std::exception& err) {
            error = label + " api key: " + err.what();
            return {};
        }
    }
    net::HttpResponse response;
    try {
        response = net::HttpClient{}.Get(request);
    } catch (const net::HttpError& err) {
        error = label + " fetch failed: " + err.what();
        return {};
    }
    if (!common::IsSuccessStatus(response.status)) {
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
    auto rates = IngestPrices(source.format, document);
    if (rates.empty()) {
        error = "the " + label + " snapshot contained no per-token-priced models (is it " +
                RateFormatName(source.format) + "-shaped?)";
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
    const auto& sources = settings.rate_sources;
    if (sources.empty()) {
        outcome.error = "no rate_sources configured";
        return outcome;
    }

    // Fetched CONCURRENTLY, not sequentially: each is an independent
    // blocking HTTP GET (own socket, own net::HttpClient instance -- no
    // shared state between the calls), so running them one after the other
    // would sum worst-case latency for no reason. This matters most on the
    // paths that block a human or the whole process on the result: startup
    // auto-refresh, `--plugin-call cost-tracker refresh_rates`, and the `.rpc
    // refresh_rates` handler thread all run through here.
    std::vector<std::map<std::string, Rate>> fetched(sources.size());
    outcome.sources.resize(sources.size());
    {
        std::vector<std::thread> workers;
        workers.reserve(sources.size());
        const auto fetch = [&](std::size_t i) {
            fetched[i] = FetchSource(sources[i], settings.rates_fetch_timeout_ms,
                                     outcome.sources[i].error);
        };
        for (std::size_t i = 0; i < sources.size(); ++i) {
            outcome.sources[i].name = sources[i].name;
            // A thread that cannot be started (EAGAIN under a thread limit)
            // fetches inline instead: unwinding past the joinable workers
            // already started would std::terminate the server.
            try {
                workers.emplace_back(fetch, i);
            } catch (const std::system_error&) {
                fetch(i);
            }
        }
        for (auto& worker : workers) worker.join();
    }

    outcome.fetched_at = Iso8601UtcNow();
    const std::int64_t fetched_at_unix = UnixSecondsNow();
    // Each cache write is its own try/catch: whichever snapshot persists
    // takes effect regardless of what happens to the others -- one shared
    // try once meant a written snapshot never reached the live RateTable
    // because a LATER write threw before RefreshFetched() ran.
    for (std::size_t i = 0; i < sources.size(); ++i) {
        if (fetched[i].empty()) continue;
        auto& result = outcome.sources[i];
        try {
            FetchedRates snapshot;
            snapshot.fetched_at = outcome.fetched_at;
            snapshot.fetched_at_unix = fetched_at_unix;
            const int count = static_cast<int>(fetched[i].size());
            snapshot.models = std::move(fetched[i]);
            store.SetFetchedRates(result.name, snapshot);
            result.updated = count;
            outcome.updated += count;
        } catch (const std::exception& err) {
            result.error = "caching the " + result.name + " rates failed: " + err.what();
        }
    }

    const bool any_stored = outcome.updated > 0;
    if (any_stored) rates.RefreshFetched();
    outcome.ok = any_stored;
    // A degraded success: each fresh tier takes effect while the failed ones
    // stay stale -- the same rule a total failure obeys, just per source.
    // Fetch errors and store errors are both reported here.
    std::string notes;
    for (const auto& result : outcome.sources) {
        if (result.error.empty()) continue;
        notes += notes.empty() ? result.error : "; " + result.error;
    }
    if (!any_stored) {
        outcome.fetched_at.clear();
        outcome.error = notes;
    } else if (!notes.empty()) {
        outcome.warning = notes;
    }
    return outcome;
}

}  // namespace minimal::cost_tracker
