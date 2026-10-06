// plugins.json and the per-plugin config files beside it (plugins.d/):
// parsing, degradation, the serialize round-trip the startup rewrite relies
// on, and discovery carrying each plugin's file onto its candidate.
#include <catch2/catch_test_macros.hpp>

#include <unistd.h>

#include <filesystem>
#include <fstream>

#include "minimal/plugins/plugin_discovery.hpp"
#include "temp_dir.hpp"

using namespace minimal;
using minimal::testing::TempDir;
using plugins::ParsePluginConfig;
using plugins::PluginConfig;
using plugins::Provenance;
using plugins::SerializePluginConfig;

namespace {

// A bundled dir holding `modules`, and an empty plugins.d beside it.
struct Layout {
    TempDir root;
    std::filesystem::path bundled = root.path() / "bundled";
    std::filesystem::path config_dir = root.path() / "plugins.d";

    explicit Layout(std::initializer_list<const char*> modules) {
        std::filesystem::create_directories(bundled);
        std::filesystem::create_directories(config_dir);
        for (const char* module : modules) std::ofstream(bundled / module) << "x";
    }
    void Write(const std::string& name, const std::string& text) const {
        std::ofstream(config_dir / name) << text;
    }
};

}  // namespace

TEST_CASE("an inline config object is refused, naming the file it moved to",
          "[plugin_config]") {
    // Per-plugin config lives in plugins.d/<stem>.json. A leftover "config"
    // is a named problem rather than a silently ignored key: the plugin
    // would otherwise load with every default while the user believed their
    // settings applied.
    const auto config = ParsePluginConfig(R"({
      "plugins": [
        {"file": "tools-bash.so", "provenance": "bundled", "enabled": true,
         "config": {"timeout_s": 30}}
      ]
    })");
    CHECK(config.plugins.empty());
    REQUIRE(config.problems.size() == 1);
    CHECK(config.problems[0].find("plugins.json: plugins[0].config") != std::string::npos);
    CHECK(config.problems[0].find("plugins.d/tools-bash.json") != std::string::npos);
}

TEST_CASE("a wrong-typed entry is named and dropped; later entries still parse",
          "[plugin_config][regression]") {
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
    // A hand-written {"file": ...} used to be read as a USER plugin that is
    // DISABLED. Now the file name claims the plugin and the default applies.
    const Layout layout({"alpha.so"});
    auto config = ParsePluginConfig(R"({"plugins": [{"file": "alpha.so"}]})");
    const auto candidates = plugins::DiscoverPlugins(layout.bundled, {}, config);
    REQUIRE(candidates.size() == 1);
    CHECK(candidates[0].enabled);
    REQUIRE(config.plugins.size() == 1);
    CHECK(config.plugins[0].provenance == Provenance::Bundled);
    CHECK(config.plugins[0].enabled == true);
    CHECK(config.problems.empty());
}

TEST_CASE("an entry without provenance is ambiguous when both sides ship the name",
          "[plugin_discovery]") {
    const Layout layout({"alpha.so"});
    const auto user = layout.root.path() / "user";
    std::filesystem::create_directories(user);
    { std::ofstream(user / "alpha.so") << "x"; }

    auto config = ParsePluginConfig(R"({"plugins": [{"file": "alpha.so", "enabled": true}]})");
    (void)plugins::DiscoverPlugins(layout.bundled, user, config);
    REQUIRE(config.problems.size() == 1);
    CHECK(config.problems[0].find("names no provenance") != std::string::npos);
}

TEST_CASE("serialization round-trips entries, aliases and unmodelled keys", "[plugin_config]") {
    // main.cpp rewrites plugins.json on every startup.
    PluginConfig config;
    PluginConfig::Entry bash;
    bash.file = "tools-bash.so";
    bash.provenance = Provenance::Bundled;
    bash.enabled = true;
    config.plugins.push_back(bash);
    PluginConfig::Entry custom;
    custom.file = "custom.so";
    custom.provenance = Provenance::User;
    custom.enabled = false;
    config.plugins.push_back(custom);
    config.aliases["custom.tool"] = "tool";
    config.preserved["future_key"] = true;

    const std::string saved = SerializePluginConfig(config);
    CHECK(saved.find("\"config\"") == std::string::npos);  // no inline config, ever
    const auto reparsed = ParsePluginConfig(saved);
    CHECK(reparsed.problems.empty());
    REQUIRE(reparsed.plugins.size() == 2);
    CHECK(reparsed.plugins[1].enabled == false);
    CHECK(reparsed.aliases.at("custom.tool") == "tool");
    CHECK(reparsed.preserved.contains("future_key"));
    CHECK(SerializePluginConfig(reparsed) == saved);  // idempotent
}

TEST_CASE("discovery carries each plugin's plugins.d file onto its candidate",
          "[plugin_discovery]") {
    // The candidate is what the loader sees: this is the wire a plugin's
    // config rides from its file to the registration context.
    const Layout layout({"alpha.so", "beta.so"});
    layout.Write("alpha.json", R"({"echo_value": "from plugins.d"})");

    PluginConfig config;
    const auto candidates =
        plugins::DiscoverPlugins(layout.bundled, {}, config, layout.config_dir);
    REQUIRE(candidates.size() == 2);
    CHECK(candidates[0].config == nlohmann::json{{"echo_value", "from plugins.d"}});
    CHECK_FALSE(candidates[0].config_error.has_value());
    CHECK(candidates[1].config == nlohmann::json::object());  // no file: {}
    CHECK_FALSE(candidates[1].config_error.has_value());
}

TEST_CASE("an unreadable plugins.d file is the candidate's error, never a silent {}",
          "[plugin_discovery]") {
    const Layout layout({"alpha.so", "beta.so", "gamma.so"});
    layout.Write("alpha.json", "{ not json");
    layout.Write("beta.json", "[1, 2, 3]");
    layout.Write("gamma.json", "");  // empty reads as absent

    PluginConfig config;
    const auto candidates =
        plugins::DiscoverPlugins(layout.bundled, {}, config, layout.config_dir);
    REQUIRE(candidates.size() == 3);
    REQUIRE(candidates[0].config_error.has_value());
    CHECK(candidates[0].config_error->find("plugins.d/alpha.json: invalid JSON") !=
          std::string::npos);
    REQUIRE(candidates[1].config_error.has_value());
    CHECK(candidates[1].config_error->find("plugins.d/beta.json: must be a JSON object") !=
          std::string::npos);
    CHECK_FALSE(candidates[2].config_error.has_value());
    CHECK(candidates[2].config == nlohmann::json::object());
}

TEST_CASE("a disabled plugin's plugins.d file is not read", "[plugin_discovery]") {
    // A broken file must not fail a plugin the user switched off.
    const Layout layout({"alpha.so"});
    layout.Write("alpha.json", "{ not json");
    auto config = ParsePluginConfig(
        R"({"plugins": [{"file": "alpha.so", "provenance": "bundled", "enabled": false}]})");
    const auto candidates =
        plugins::DiscoverPlugins(layout.bundled, {}, config, layout.config_dir);
    REQUIRE(candidates.size() == 1);
    CHECK_FALSE(candidates[0].enabled);
    CHECK_FALSE(candidates[0].config_error.has_value());
}
