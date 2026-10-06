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

TEST_CASE("a non-object config is a named problem, not an empty object",
          "[plugin_config][track_k]") {
    // Degrading it let the plugin load with every default while the user
    // believed their settings applied.
    const std::string text = R"({
      "plugins": [
        {"file": "a.so", "config": [1, 2, 3]},
        {"file": "b.so", "config": "timeout_ms=30"}
      ]
    })";

    const auto config = ParsePluginConfig(text);
    CHECK(config.plugins.empty());
    REQUIRE(config.problems.size() == 2);
    CHECK(config.problems[0].find("plugins.json: plugins[0].config: must be an object") !=
          std::string::npos);
}

TEST_CASE("a wrong-typed entry is named and dropped; later entries still parse",
          "[plugin_config][track_k][regression]") {
    const std::string text = R"({
      "plugins": [
        {"file": "a.so", "enabled": "true"},
        {"file": "b.so", "provenance": 1, "enabled": true},
        {"file": 42, "enabled": true},
        {"file": "d.so", "enabled": true}
      ]
    })";

    const auto config = ParsePluginConfig(text);
    REQUIRE(config.plugins.size() == 1);
    CHECK(config.plugins[0].file == "d.so");
    CHECK(config.plugins[0].enabled == true);
    REQUIRE(config.problems.size() == 3);
    CHECK(config.problems[2].find("plugins[2].file: must be a string") != std::string::npos);
}

TEST_CASE("an entry without provenance or enabled resolves at discovery",
          "[plugin_discovery]") {
    // The hand-written {"file": ..., "config": ...} used to be read as a USER
    // plugin that is DISABLED -- so a bundled plugin's config was silently
    // orphaned. Now the file name claims the plugin and the default applies.
    const auto tmp = std::filesystem::temp_directory_path() /
                     ("minimal-discovery-resolve-" + std::to_string(::getpid()));
    const auto bundled = tmp / "bundled";
    std::filesystem::create_directories(bundled);
    { std::ofstream(bundled / "alpha.so") << "x"; }

    auto config = ParsePluginConfig(
        R"({"plugins": [{"file": "alpha.so", "config": {"echo_value": "mine"}}]})");
    const auto candidates = plugins::DiscoverPlugins(bundled, {}, config);
    REQUIRE(candidates.size() == 1);
    CHECK(candidates[0].enabled);
    CHECK(candidates[0].config == nlohmann::json{{"echo_value", "mine"}});
    REQUIRE(config.plugins.size() == 1);
    CHECK(config.plugins[0].provenance == Provenance::Bundled);
    CHECK(config.plugins[0].enabled == true);
    CHECK(config.problems.empty());

    std::filesystem::remove_all(tmp);
}

TEST_CASE("an entry without provenance is ambiguous when both sides ship the name",
          "[plugin_discovery]") {
    const auto tmp = std::filesystem::temp_directory_path() /
                     ("minimal-discovery-ambiguous-" + std::to_string(::getpid()));
    const auto bundled = tmp / "bundled";
    const auto user = tmp / "user";
    std::filesystem::create_directories(bundled);
    std::filesystem::create_directories(user);
    { std::ofstream(bundled / "alpha.so") << "x"; }
    { std::ofstream(user / "alpha.so") << "x"; }

    auto config = ParsePluginConfig(R"({"plugins": [{"file": "alpha.so", "enabled": true}]})");
    (void)plugins::DiscoverPlugins(bundled, user, config);
    REQUIRE(config.problems.size() == 1);
    CHECK(config.problems[0].find("names no provenance") != std::string::npos);

    std::filesystem::remove_all(tmp);
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
