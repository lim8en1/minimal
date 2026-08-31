// plugins.json per-plugin config (track K): parsing, degradation, and the
// serialize round-trip that protects user config from the startup rewrite.
#include <catch2/catch_test_macros.hpp>

#include <unistd.h>

#include <filesystem>
#include <fstream>

#include "minimal/plugins/plugin_discovery.hpp"

using namespace minimal;
using plugins::ParsePluginConfig;
using plugins::PluginConfig;
using plugins::Provenance;
using plugins::SerializePluginConfig;

TEST_CASE("plugins.json entries parse their config object", "[plugin_config][track_k]") {
    const std::string text = R"({
      "plugins": [
        {"file": "tools-bash.so", "provenance": "bundled", "enabled": true,
         "config": {"timeout_ms": 30000, "shell": "/bin/dash"}}
      ]
    })";

    const auto config = ParsePluginConfig(text);
    REQUIRE(config.plugins.size() == 1);
    CHECK(config.plugins[0].config ==
          nlohmann::json{{"timeout_ms", 30000}, {"shell", "/bin/dash"}});
}

TEST_CASE("a plugin entry without config gets an empty object", "[plugin_config][track_k]") {
    const std::string text =
        R"({"plugins": [{"file": "a.so", "provenance": "user", "enabled": true}]})";

    const auto config = ParsePluginConfig(text);
    REQUIRE(config.plugins.size() == 1);
    CHECK(config.plugins[0].config == nlohmann::json::object());
}

TEST_CASE("a non-object config degrades to an empty object", "[plugin_config][track_k]") {
    // Degrade-don't-throw, like the rest of this file: the plugin still loads,
    // it just sees no config. (The startup warning is logged; not asserted here.)
    const std::string text = R"({
      "plugins": [
        {"file": "a.so", "config": [1, 2, 3]},
        {"file": "b.so", "config": "timeout_ms=30"},
        {"file": "c.so", "config": 42}
      ]
    })";

    const auto config = ParsePluginConfig(text);
    REQUIRE(config.plugins.size() == 3);
    for (const auto& entry : config.plugins) {
        CHECK(entry.config == nlohmann::json::object());
    }
}

TEST_CASE("serialization round-trips config through a load->save->load cycle",
          "[plugin_config][track_k]") {
    // main.cpp rewrites plugins.json on every startup. A config that vanishes
    // on the next save is user config silently dropped.
    PluginConfig config;
    PluginConfig::Entry configured;
    configured.file = "tools-bash.so";
    configured.provenance = Provenance::Bundled;
    configured.enabled = true;
    configured.config = nlohmann::json{{"timeout_ms", 30000}, {"shell", "/bin/dash"}};
    config.plugins.push_back(configured);

    PluginConfig::Entry unconfigured;
    unconfigured.file = "custom.so";
    unconfigured.provenance = Provenance::User;
    unconfigured.enabled = false;
    config.plugins.push_back(unconfigured);

    const auto reparsed = ParsePluginConfig(SerializePluginConfig(config));

    REQUIRE(reparsed.plugins.size() == 2);
    CHECK(reparsed.plugins[0].config ==
          nlohmann::json{{"timeout_ms", 30000}, {"shell", "/bin/dash"}});
    // A second cycle is idempotent: what was saved parses to what saves again.
    const auto third = ParsePluginConfig(SerializePluginConfig(reparsed));
    CHECK(third.plugins[0].config == reparsed.plugins[0].config);
    CHECK(third.plugins[1].config == nlohmann::json::object());
}

TEST_CASE("discovery carries the entry's config onto the candidate",
          "[plugin_discovery][track_k]") {
    // The candidate is what the loader sees, so this is the wire the config
    // rides from plugins.json to the registration context.
    const auto tmp = std::filesystem::temp_directory_path() /
                     ("minimal-discovery-config-" + std::to_string(::getpid()));
    const auto bundled = tmp / "bundled";
    std::filesystem::create_directories(bundled);
    { std::ofstream(bundled / "alpha.so") << "x"; }

    const std::string text = R"({
      "plugins": [
        {"file": "alpha.so", "provenance": "bundled", "enabled": true,
         "config": {"echo_value": "from plugins.json"}}
      ]
    })";
    auto config = ParsePluginConfig(text);

    const auto candidates = plugins::DiscoverPlugins(bundled, {}, config);

    REQUIRE(candidates.size() == 1);
    CHECK(candidates[0].config == nlohmann::json{{"echo_value", "from plugins.json"}});

    std::filesystem::remove_all(tmp);
}

TEST_CASE("discovery preserves an existing entry's config when merging",
          "[plugin_discovery][track_k]") {
    // Newly discovered entries are appended and written back; a pre-existing
    // entry (and its config) must survive that merge untouched.
    const auto tmp = std::filesystem::temp_directory_path() /
                     ("minimal-discovery-config-merge-" + std::to_string(::getpid()));
    const auto bundled = tmp / "bundled";
    std::filesystem::create_directories(bundled);
    { std::ofstream(bundled / "alpha.so") << "x"; }
    { std::ofstream(bundled / "beta.so") << "x"; }

    auto config = ParsePluginConfig(R"({
      "plugins": [
        {"file": "alpha.so", "provenance": "bundled", "enabled": true,
         "config": {"timeout_ms": 30000}}
      ]
    })");

    const auto candidates = plugins::DiscoverPlugins(bundled, {}, config);

    REQUIRE(candidates.size() == 2);
    REQUIRE(config.plugins.size() == 2);
    CHECK(config.plugins[0].config == nlohmann::json{{"timeout_ms", 30000}});
    CHECK(config.plugins[1].config == nlohmann::json::object());  // newly discovered

    // And the writeback keeps both.
    const auto reparsed = ParsePluginConfig(plugins::SerializePluginConfig(config));
    REQUIRE(reparsed.plugins.size() == 2);
    CHECK(reparsed.plugins[0].config == nlohmann::json{{"timeout_ms", 30000}});

    std::filesystem::remove_all(tmp);
}
