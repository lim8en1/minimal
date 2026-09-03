#include <catch2/catch_test_macros.hpp>

#include <sqlite3.h>
#include <unistd.h>

#include <string>
#include <thread>
#include <vector>

#include "minimal/plugins/plugin_storage.hpp"
#include "temp_dir.hpp"

using namespace minimal;
using minimal::testing::TempDir;

namespace {

struct Fixture {
    TempDir dir;

    plugins::PluginStorage Make() { return plugins::PluginStorage(dir.path() / "plugins.db"); }
};

// Opens the same file with raw sqlite to prepare states PluginStorage refuses
// to build itself (a newer schema version).
void SetUserVersion(const std::filesystem::path& path, int version) {
    sqlite3* db = nullptr;
    REQUIRE(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
    const std::string sql = "PRAGMA user_version = " + std::to_string(version) + ";";
    char* err = nullptr;
    REQUIRE(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) == SQLITE_OK);
    if (err) sqlite3_free(err);
    REQUIRE(sqlite3_close(db) == SQLITE_OK);
}

}  // namespace

// Regression: ExclusiveFileLock used to open db_path + ".lock" BEFORE the
// parent directory was created -- open()'s O_CREAT only covers the leaf
// file, not a missing directory component, so a genuinely fresh
// XDG_DATA_HOME (nothing has ever run on this machine) threw "cannot open
// lock file .../plugins.db.lock: No such file or directory" instead of just
// working. session_database.cpp already got this right; PluginStorage's
// constructor now creates the directory first, same order.
TEST_CASE("PluginStorage creates its own parent directory on first use",
          "[plugin_storage]") {
    TempDir dir;
    const auto nested = dir.path() / "fresh" / "data" / "dir";
    REQUIRE_FALSE(std::filesystem::exists(nested));

    auto storage = plugins::PluginStorage(nested / "plugins.db");
    storage.Set("checklist", "s1", "value");
    CHECK(storage.Get("checklist", "s1") == std::optional<std::string>("value"));
}

TEST_CASE("set/get round-trips per plugin and key", "[plugin_storage]") {
    Fixture f;
    auto storage = f.Make();

    storage.Set("checklist", "s1", R"({"items":[]})");
    storage.Set("checklist", "s2", "second");
    storage.Set("other", "s1", "other-plugin-value");

    REQUIRE(storage.Get("checklist", "s1") == std::optional<std::string>(R"({"items":[]})"));
    REQUIRE(storage.Get("checklist", "s2") == std::optional<std::string>("second"));
    // The namespace is the boundary: same key, different plugin, different row.
    REQUIRE(storage.Get("other", "s1") == std::optional<std::string>("other-plugin-value"));
    REQUIRE_FALSE(storage.Get("checklist", "absent").has_value());
    REQUIRE_FALSE(storage.Get("no-such-plugin", "s1").has_value());
}

TEST_CASE("set upserts and updates updated_at", "[plugin_storage]") {
    Fixture f;
    auto storage = f.Make();

    storage.Set("p", "k", "one");
    storage.Set("p", "k", "two");
    REQUIRE(storage.Get("p", "k") == std::optional<std::string>("two"));

    // Still one row per (plugin, key) -- an upsert, not an append.
    REQUIRE(storage.Keys("p").size() == 1);
}

TEST_CASE("delete removes exactly one row and reports whether it did", "[plugin_storage]") {
    Fixture f;
    auto storage = f.Make();

    storage.Set("p", "k", "v");
    CHECK(storage.Delete("p", "k"));
    CHECK_FALSE(storage.Delete("p", "k"));          // second delete: nothing to remove
    CHECK_FALSE(storage.Delete("p", "never-was"));  // absent: false, not an error
    CHECK_FALSE(storage.Get("p", "k").has_value());
}

TEST_CASE("keys lists the plugin's own keys, prefix-filtered and sorted", "[plugin_storage]") {
    Fixture f;
    auto storage = f.Make();

    storage.Set("p", "session-b", "3");
    storage.Set("p", "session-a", "1");
    storage.Set("p", "other", "0");
    storage.Set("p", "session-a/child", "2");
    storage.Set("q", "session-a", "other plugin");

    const auto all = storage.Keys("p");
    REQUIRE(all == std::vector<std::string>{"other", "session-a", "session-a/child", "session-b"});

    // Range scan on the PK, and scoped to the namespace: q's row must not appear.
    const auto prefixed = storage.Keys("p", "session-a");
    REQUIRE(prefixed == std::vector<std::string>{"session-a", "session-a/child"});
    REQUIRE(storage.Keys("p", "no-match").empty());
}

TEST_CASE("delete-prefix removes the range and nothing else", "[plugin_storage]") {
    Fixture f;
    auto storage = f.Make();

    storage.Set("p", "s1/a", "1");
    storage.Set("p", "s1/b", "2");
    storage.Set("p", "s2/a", "3");
    storage.Set("p", "keep", "4");

    REQUIRE(storage.DeletePrefix("p", "s1/") == 2);
    REQUIRE(storage.Keys("p") == std::vector<std::string>{"keep", "s2/a"});
}

TEST_CASE("state survives close and reopen", "[plugin_storage]") {
    Fixture f;
    {
        auto storage = f.Make();
        storage.Set("checklist", "s1", "persisted");
    }
    auto reopened = f.Make();
    REQUIRE(reopened.Get("checklist", "s1") == std::optional<std::string>("persisted"));
}

TEST_CASE("a database written by a newer binary refuses to open", "[plugin_storage]") {
    Fixture f;
    const auto path = f.dir.path() / "plugins.db";
    f.Make();  // create + migrate to the current version
    SetUserVersion(path, 99);

    try {
        f.Make();
        FAIL("expected open to be refused");
    } catch (const std::exception& e) {
        CHECK(std::string(e.what()).find("newer than this binary understands") !=
              std::string::npos);
    }
}

TEST_CASE("a second open of the same storage is refused, holder pid named",
          "[plugin_storage]") {
    // Same single-writer guarantee as the session store, same mechanism: the
    // lock file names the holder so the operator can find it, and the lock
    // dies with the process so a crash leaves no staleness.
    Fixture f;
    auto storage = f.Make();

    bool threw = false;
    std::string message;
    try {
        auto second = f.Make();
    } catch (const std::exception& e) {
        threw = true;
        message = e.what();
    }
    CHECK(threw);
    CHECK(message.find(std::to_string(getpid())) != std::string::npos);
}

TEST_CASE("concurrent plugins share one connection safely", "[plugin_storage]") {
    Fixture f;
    auto storage = f.Make();

    // Two threads, two namespaces, the same keys: the per-call mutex is what
    // makes concurrent session workers + command pool + observers safe.
    constexpr int kIterations = 200;
    std::thread a([&] {
        for (int i = 0; i < kIterations; ++i) storage.Set("a", "k" + std::to_string(i % 8), "a");
    });
    std::thread b([&] {
        for (int i = 0; i < kIterations; ++i) storage.Set("b", "k" + std::to_string(i % 8), "b");
    });
    a.join();
    b.join();

    for (int i = 0; i < 8; ++i) {
        REQUIRE(storage.Get("a", "k" + std::to_string(i)) == std::optional<std::string>("a"));
        REQUIRE(storage.Get("b", "k" + std::to_string(i)) == std::optional<std::string>("b"));
    }
}

TEST_CASE("empty plugin or key names are rejected", "[plugin_storage]") {
    Fixture f;
    auto storage = f.Make();

    CHECK_THROWS_AS(storage.Set("", "k", "v"), std::invalid_argument);
    CHECK_THROWS_AS(storage.Set("p", "", "v"), std::invalid_argument);
    CHECK_THROWS_AS(storage.Get("p", ""), std::invalid_argument);
    // An empty PREFIX is valid (all keys); an empty namespace is not.
    CHECK(storage.Keys("p").empty());
    CHECK_THROWS_AS(storage.Keys("", "x"), std::invalid_argument);
}
