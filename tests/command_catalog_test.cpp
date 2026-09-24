#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "minimal/client/command_catalog.hpp"

using minimal::client::CommandCatalogCache;
using minimal::client::CommandCatalogEntry;
using Entries = CommandCatalogCache::Entries;

namespace {

// A fetch the test completes by hand, recording every request.
struct ManualFetch {
    struct Request {
        std::string session_id;
        std::string project_root;
        std::function<void(std::optional<Entries>)> done;
    };
    std::vector<Request> requests;
    CommandCatalogCache::Fetch fn() {
        return [this](const std::string& session_id, const std::string& project_root,
                      std::function<void(std::optional<Entries>)> done) {
            requests.push_back({session_id, project_root, std::move(done)});
        };
    }
};

struct FakeClock {
    CommandCatalogCache::Clock::time_point t{};
    std::function<CommandCatalogCache::Clock::time_point()> fn() {
        return [this] { return t; };
    }
};

Entries Named(std::initializer_list<const char*> names) {
    Entries out;
    for (const char* name : names) out.push_back({name, "", false, false});
    return out;
}

std::vector<std::string> NamesOf(const CommandCatalogCache::Snapshot& snapshot) {
    std::vector<std::string> out;
    for (const auto& entry : *snapshot) out.push_back(entry.name);
    return out;
}

}  // namespace

TEST_CASE("CommandCatalogCache: answers from the seed at once, refreshes in the background",
          "[command_catalog]") {
    ManualFetch fetch;
    FakeClock clock;
    CommandCatalogCache cache(Named({"new"}), fetch.fn(), std::chrono::seconds(2), clock.fn());
    cache.Seed({}, "/repo", Named({"help"}));

    // Seeded scope inside the refresh window: the seed, no fetch.
    CHECK(NamesOf(cache.Get({}, "/repo")) == std::vector<std::string>{"help", "new"});
    CHECK(fetch.requests.empty());

    // Past the window: still answers immediately, and one fetch starts.
    clock.t += std::chrono::seconds(3);
    const auto before = cache.Get({}, "/repo");
    CHECK(NamesOf(before) == std::vector<std::string>{"help", "new"});
    REQUIRE(fetch.requests.size() == 1);
    CHECK(fetch.requests[0].project_root == "/repo");
    cache.Get({}, "/repo");   // in flight: no second fetch
    CHECK(fetch.requests.size() == 1);

    fetch.requests[0].done(Named({"help", "deploy"}));
    const auto after = cache.Get({}, "/repo");
    CHECK(NamesOf(after) == std::vector<std::string>{"help", "deploy", "new"});
    CHECK(after != before);                                        // a new snapshot
    CHECK(NamesOf(before) == std::vector<std::string>{"help", "new"});   // old one intact
    CHECK(fetch.requests.size() == 1);   // just refreshed: inside the window again
}

TEST_CASE("CommandCatalogCache: a session scope fetches by session id and falls back to the "
          "seed until it lands",
          "[command_catalog]") {
    ManualFetch fetch;
    FakeClock clock;
    CommandCatalogCache cache({}, fetch.fn(), std::chrono::seconds(2), clock.fn());
    cache.Seed({}, "/repo", Named({"help"}));

    CHECK(NamesOf(cache.Get("s1", "")) == std::vector<std::string>{"help"});   // seed fallback
    REQUIRE(fetch.requests.size() == 1);
    CHECK(fetch.requests[0].session_id == "s1");
    fetch.requests[0].done(Named({"other"}));
    CHECK(NamesOf(cache.Get("s1", "")) == std::vector<std::string>{"other"});
    CHECK(NamesOf(cache.Get({}, "/repo")) == std::vector<std::string>{"help"});   // untouched
}

TEST_CASE("CommandCatalogCache: a failed fetch keeps the last list and retries after the "
          "window; Listed reflects whether any server list exists",
          "[command_catalog]") {
    ManualFetch fetch;
    FakeClock clock;
    CommandCatalogCache cache(Named({"new"}), fetch.fn(), std::chrono::seconds(2), clock.fn());

    // No seed (connect-time fetch failed): client-side entries only.
    CHECK(NamesOf(cache.Get({}, "/repo")) == std::vector<std::string>{"new"});
    CHECK_FALSE(cache.Listed({}, "/repo"));
    REQUIRE(fetch.requests.size() == 1);
    fetch.requests[0].done(std::nullopt);
    CHECK(NamesOf(cache.Get({}, "/repo")) == std::vector<std::string>{"new"});
    CHECK(fetch.requests.size() == 1);   // inside the window: no hammering

    clock.t += std::chrono::seconds(3);
    cache.Get({}, "/repo");
    REQUIRE(fetch.requests.size() == 2);
    fetch.requests[1].done(Named({"help"}));
    CHECK(cache.Listed({}, "/repo"));
    CHECK(NamesOf(cache.Get({}, "/repo")) == std::vector<std::string>{"help", "new"});
}

TEST_CASE("CommandCatalogCache: a completion landing after the cache is gone is dropped",
          "[command_catalog]") {
    ManualFetch fetch;
    {
        CommandCatalogCache cache({}, fetch.fn());
        cache.Get({}, "/repo");
    }
    REQUIRE(fetch.requests.size() == 1);
    fetch.requests[0].done(Named({"late"}));   // must not touch freed state
    SUCCEED();
}

TEST_CASE("CommandCatalogCache: a synchronous fetch completes inside Get without deadlock",
          "[command_catalog]") {
    CommandCatalogCache cache(
        {}, [](const std::string&, const std::string&,
               std::function<void(std::optional<Entries>)> done) { done(Named({"now"})); });
    cache.Get({}, "/repo");   // starts and completes the fetch
    CHECK(NamesOf(cache.Get({}, "/repo")) == std::vector<std::string>{"now"});
}
