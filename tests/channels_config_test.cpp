#include <catch2/catch_test_macros.hpp>

#include "minimal/mcp/channels_config.hpp"

using namespace minimal::config;

namespace {

nlohmann::json RestrictedEntry() {
    return {{"mode", "restricted"},
            {"signals", {"build.completed", "build.failed"}},
            {"command", "bun"},
            {"args", {"./ci-channel.ts"}},
            {"env", {{"CI_TOKEN", "${CI_TOKEN}"}}}};
}

nlohmann::json ExtendedEntry() { return {{"mode", "extended"}, {"command", "./reminder"}}; }

}  // namespace

TEST_CASE("ParseChannelsConfig: restricted entry decodes every field", "[channels_config]") {
    auto config = ParseChannelsConfig(nlohmann::json{{"ci", RestrictedEntry()}});

    REQUIRE(config.errors.empty());
    REQUIRE(config.entries.size() == 1);
    const auto& entry = config.entries.front();
    CHECK(entry.name == "ci");  // the JSON key IS the name
    CHECK_FALSE(entry.extended);
    CHECK(entry.signals == std::vector<std::string>{"build.completed", "build.failed"});
    CHECK(entry.command == "bun");
    CHECK(entry.args == std::vector<std::string>{"./ci-channel.ts"});
    CHECK(entry.env.at("CI_TOKEN") == "${CI_TOKEN}");  // raw; interpolation is connect-time
    CHECK(entry.enabled);
}

TEST_CASE("ParseChannelsConfig: extended entry, mode defaults to restricted",
          "[channels_config]") {
    // No mode field: the safe mode is the easy mode.
    auto config = ParseChannelsConfig(nlohmann::json{{"reminder", ExtendedEntry()}});
    REQUIRE(config.errors.empty());
    REQUIRE(config.entries.size() == 1);
    CHECK(config.entries.front().extended);
    CHECK(config.entries.front().signals.empty());

    auto defaulted = ParseChannelsConfig(nlohmann::json{{"ci", {{"command", "c"}}}});
    // Defaulting to restricted also enforces its obligation: no signals list
    // means the entry is REFUSED (recorded, not added), not a silent dead
    // channel.
    REQUIRE(defaulted.errors.size() == 1);
    CHECK(defaulted.errors.front().find("non-empty signals") != std::string::npos);
    CHECK(defaulted.entries.empty());
}

TEST_CASE("ParseChannelsConfig: a non-array 'args' is a named error, not a silently reordered list",
          "[channels_config][regression]") {
    // The bug: entry.at("args") was iterated without an is_array() check.
    // nlohmann's range-for over a JSON OBJECT iterates its VALUES in
    // key-sorted order, so a typo'd object silently became an args list in
    // the wrong order instead of the intended rejection.
    auto entry = RestrictedEntry();
    entry["args"] = nlohmann::json{{"1", "--foo"}, {"0", "--bar"}};
    auto config = ParseChannelsConfig(nlohmann::json{{"ci", entry}});
    CHECK(config.entries.empty());
    REQUIRE(config.errors.size() == 1);
    CHECK(config.errors[0].find("args") != std::string::npos);
}

TEST_CASE("ParseChannelsConfig: per-entry errors degrade, never throw", "[channels_config]") {
    auto config = ParseChannelsConfig(nlohmann::json{
        {"bad-signal",
         {{"mode", "restricted"}, {"signals", {"BUILD"}}, {"command", "c"}}},  // charset
        {"empty-signals",
         {{"mode", "restricted"}, {"signals", nlohmann::json::array()}, {"command", "c"}}},
        {"extended-with-signals",
         {{"mode", "extended"}, {"signals", {"x"}}, {"command", "c"}}},
        {"no-command", {{"mode", "extended"}}},
        {"bad-mode", {{"mode", "silent"}, {"command", "c"}}},
        {"good", ExtendedEntry()}});

    // Exactly the five bad ones, each named by its channel key.
    REQUIRE(config.errors.size() == 5);
    CHECK(config.entries.size() == 1);
    CHECK(config.entries.front().name == "good");
    bool named_bad_charset = false, named_empty = false, named_extended = false,
         named_no_command = false, named_bad_mode = false;
    for (const auto& error : config.errors) {
        named_bad_charset |= error.find("bad-signal") != std::string::npos;
        named_empty |= error.find("empty-signals") != std::string::npos &&
                       error.find("non-empty signals") != std::string::npos;
        named_extended |= error.find("extended-with-signals") != std::string::npos &&
                          error.find("must not declare signals") != std::string::npos;
        named_no_command |= error.find("no-command") != std::string::npos;
        named_bad_mode |= error.find("bad-mode") != std::string::npos;
    }
    CHECK(named_bad_charset);
    CHECK(named_empty);
    CHECK(named_extended);
    CHECK(named_no_command);
    CHECK(named_bad_mode);
}

TEST_CASE("ParseChannelsConfig: reserved internal channel name is refused", "[channels_config]") {
    auto config = ParseChannelsConfig(nlohmann::json{{"sessions", ExtendedEntry()}});
    REQUIRE(config.errors.size() == 1);
    CHECK(config.errors.front().find("reserved") != std::string::npos);
    // The name passes the charset rule on its own; it is refused because the
    // internal cross-session source is not addressable by config.
}

TEST_CASE("ParseChannelsConfig: file-level problems throw, per-entry ones never do",
          "[channels_config]") {
    // Top level: the one throwing shape.
    CHECK_THROWS(ParseChannelsConfig(nlohmann::json::array()));
    // A non-object ENTRY is per-entry: recorded, named, others unaffected.
    auto config = ParseChannelsConfig(nlohmann::json{{"ci", "not an object"}, {"ok", ExtendedEntry()}});
    REQUIRE(config.errors.size() == 1);
    CHECK(config.errors.front().find("channels.json: ci: must be an object") != std::string::npos);
    REQUIRE(config.entries.size() == 1);
    CHECK(config.entries.front().name == "ok");
}

TEST_CASE("LoadChannelsConfig: missing file is the normal empty state", "[channels_config]") {
    auto config = LoadChannelsConfig("/nonexistent/path/channels.json");
    CHECK(config.entries.empty());
    CHECK(config.errors.empty());
}
