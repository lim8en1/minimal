#include <catch2/catch_test_macros.hpp>

#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "minimal/llm/provider_factory_registry.hpp"
#include "minimal/plugins/plugin_discovery.hpp"
#include "minimal/plugins/plugin_loader.hpp"
#include "minimal/tools/tool_registry.hpp"

using namespace minimal;
using plugins::LoadStatus;
using plugins::PluginCandidate;
using plugins::Provenance;

namespace {

// MINIMAL_FIXTURE_DIR is injected by CMake: fixture plugins are real .so files
// that must actually be dlopen'd, since a loader cannot be honestly tested
// against anything less.
std::filesystem::path FixturePath(const std::string& name) {
    return std::filesystem::path(MINIMAL_FIXTURE_DIR) / name;
}

std::string SharedLibName(const std::string& stem) {
#if defined(__APPLE__)
    return stem + ".so";  // set_target_properties gives MODULE libs .so on Apple too
#else
    return stem + ".so";
#endif
}

PluginCandidate Enabled(const std::string& stem, Provenance provenance = Provenance::Bundled) {
    return PluginCandidate{FixturePath(SharedLibName(stem)), provenance, true};
}

struct Harness {
    tools::ToolRegistry tool_registry;
    llm::ProviderFactoryRegistry provider_registry;
    plugins::PluginLoader loader{tool_registry, provider_registry};
};

}  // namespace

TEST_CASE("a well-formed plugin loads and contributes its registrations", "[plugin_loader]") {
    Harness h;
    const auto records = h.loader.LoadAll({Enabled("fixture-good")});

    REQUIRE(records.size() == 1);
    const auto& record = records.front();
    CHECK(record.status == LoadStatus::Loaded);
    CHECK(record.name == "fixture-good");
    CHECK(record.version == "1.0.0");
    CHECK(record.error.empty());

    CHECK(record.registered_tools == std::vector<std::string>{"fixture_tool"});
    CHECK(record.registered_provider_kinds == std::vector<std::string>{"fixture"});

    // The registrations reached the real registries, not just the record.
    CHECK(h.tool_registry.Find("fixture_tool") != nullptr);
    CHECK(h.provider_registry.Has("fixture"));
    CHECK(h.provider_registry.Create("fixture", {}) != nullptr);
}

TEST_CASE("an ABI-mismatched plugin is skipped without running its code",
          "[plugin_loader][abi]") {
    Harness h;
    const auto records = h.loader.LoadAll({Enabled("fixture-bad-abi")});

    REQUIRE(records.size() == 1);
    CHECK(records.front().status == LoadStatus::AbiMismatch);
    CHECK(records.front().error.find("rebuild") != std::string::npos);

    // The gate must close before any registration callback runs: a plugin built
    // against a different toolchain must never execute against our types.
    CHECK(h.tool_registry.size() == 0);
}

TEST_CASE("one bad plugin does not prevent the others loading", "[plugin_loader]") {
    // The property that matters operationally: a stale plugin degrades the
    // harness, it does not take it down.
    Harness h;
    const auto records = h.loader.LoadAll({
        Enabled("fixture-bad-abi"),
        Enabled("fixture-good"),
        PluginCandidate{FixturePath("does-not-exist.so"), Provenance::User, true},
    });

    REQUIRE(records.size() == 3);
    CHECK(records[0].status == LoadStatus::AbiMismatch);
    CHECK(records[1].status == LoadStatus::Loaded);
    CHECK(records[2].status == LoadStatus::OpenFailed);
    CHECK_FALSE(records[2].error.empty());

    CHECK(h.tool_registry.Find("fixture_tool") != nullptr);
}

TEST_CASE("a plugin exporting no optional seams loads cleanly", "[plugin_loader]") {
    // Symbol presence is the capability declaration -- absence is not an error.
    Harness h;
    const auto records = h.loader.LoadAll({Enabled("fixture-minimal")});

    REQUIRE(records.size() == 1);
    CHECK(records.front().status == LoadStatus::Loaded);
    CHECK(records.front().registered_tools.empty());
    CHECK(records.front().registered_provider_kinds.empty());
    CHECK(h.tool_registry.size() == 0);
}

TEST_CASE("a shared object that is not a plugin is reported, not fatal",
          "[plugin_loader]") {
    Harness h;
    const auto records = h.loader.LoadAll({Enabled("fixture-not-a-plugin")});

    REQUIRE(records.size() == 1);
    CHECK(records.front().status == LoadStatus::MissingRequiredSymbol);
    CHECK_FALSE(records.front().error.empty());
}

TEST_CASE("a disabled plugin is never opened", "[plugin_loader]") {
    Harness h;
    PluginCandidate disabled = Enabled("fixture-good");
    disabled.enabled = false;

    const auto records = h.loader.LoadAll({disabled});

    REQUIRE(records.size() == 1);
    CHECK(records.front().status == LoadStatus::Disabled);
    CHECK(h.tool_registry.size() == 0);
    CHECK(h.provider_registry.kinds().empty());
}

TEST_CASE("tool registrations are attributed to the registering plugin",
          "[plugin_loader][attribution]") {
    // Attribution is what lets a later collision name both culprits.
    Harness h;
    h.loader.LoadAll({Enabled("fixture-good")});

    const auto& attributions = h.tool_registry.attributions();
    REQUIRE(attributions.size() == 1);
    CHECK(attributions.front().plugin_name == "fixture-good");
    CHECK(attributions.front().tool_name == "fixture_tool");
}

// --- plugins.json ------------------------------------------------------------

TEST_CASE("plugins.json round-trips as an object", "[plugin_config]") {
    // An earlier draft described this as a bare array while also placing
    // command_aliases at the top level; both cannot hold.
    const std::string text = R"({
      "plugins": [
        {"file": "tools-fs.so", "provenance": "bundled", "enabled": true},
        {"file": "custom.so", "provenance": "user", "enabled": false}
      ],
      "command_aliases": {"bash_over_ssh.bash": "ssh_bash"}
    })";

    const auto config = plugins::ParsePluginConfig(text);
    REQUIRE(config.plugins.size() == 2);
    CHECK(config.plugins[0].file == "tools-fs.so");
    CHECK(config.plugins[0].provenance == Provenance::Bundled);
    CHECK(config.plugins[0].enabled);
    CHECK(config.plugins[1].provenance == Provenance::User);
    CHECK_FALSE(config.plugins[1].enabled);
    REQUIRE(config.command_aliases.count("bash_over_ssh.bash") == 1);
    CHECK(config.command_aliases.at("bash_over_ssh.bash") == "ssh_bash");

    const auto reparsed = plugins::ParsePluginConfig(plugins::SerializePluginConfig(config));
    REQUIRE(reparsed.plugins.size() == 2);
    CHECK(reparsed.plugins[0].file == config.plugins[0].file);
    CHECK(reparsed.command_aliases == config.command_aliases);
}

TEST_CASE("malformed plugins.json degrades to empty rather than throwing",
          "[plugin_config]") {
    CHECK(plugins::ParsePluginConfig("{ not json").plugins.empty());
    CHECK(plugins::ParsePluginConfig("[]").plugins.empty());   // wrong shape
    CHECK(plugins::ParsePluginConfig("").plugins.empty());
}

TEST_CASE("discovery applies the provenance enablement rule", "[plugin_discovery]") {
    // Bundled came out of this build and is trusted; user was dropped in and is
    // opt-in. A mechanical rule, not a special case.
    const auto tmp = std::filesystem::temp_directory_path() /
                     ("minimal-discovery-" + std::to_string(::getpid()));
    const auto bundled = tmp / "bundled";
    const auto user = tmp / "user";
    std::filesystem::create_directories(bundled);
    std::filesystem::create_directories(user);
    { std::ofstream(bundled / "alpha.so") << "x"; }
    { std::ofstream(user / "beta.so") << "x"; }

    plugins::PluginConfig config;
    const auto candidates = plugins::DiscoverPlugins(bundled, user, config);

    REQUIRE(candidates.size() == 2);
    // Bundled sorts first, so load order is reproducible.
    CHECK(candidates[0].provenance == Provenance::Bundled);
    CHECK(candidates[0].enabled);
    CHECK(candidates[1].provenance == Provenance::User);
    CHECK_FALSE(candidates[1].enabled);

    // Newly discovered entries are written back so the user can flip them.
    REQUIRE(config.plugins.size() == 2);

    std::filesystem::remove_all(tmp);
}

TEST_CASE("an existing plugins.json entry overrides the provenance default",
          "[plugin_discovery]") {
    const auto tmp = std::filesystem::temp_directory_path() /
                     ("minimal-discovery-override-" + std::to_string(::getpid()));
    const auto bundled = tmp / "bundled";
    std::filesystem::create_directories(bundled);
    { std::ofstream(bundled / "alpha.so") << "x"; }

    plugins::PluginConfig config;
    config.plugins.push_back({"alpha.so", Provenance::Bundled, /*enabled=*/false});

    const auto candidates = plugins::DiscoverPlugins(bundled, {}, config);

    REQUIRE(candidates.size() == 1);
    CHECK_FALSE(candidates[0].enabled);  // user's explicit choice wins
    CHECK(config.plugins.size() == 1);   // not duplicated

    std::filesystem::remove_all(tmp);
}
