#include <catch2/catch_test_macros.hpp>

#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "fakes.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/llm/provider_factory_registry.hpp"
#include "minimal/plugins/name_resolution.hpp"
#include "minimal/plugins/plugin_discovery.hpp"
#include "minimal/plugins/plugin_loader.hpp"
#include "minimal/tools/tool_executor.hpp"
#include "minimal/tools/tool_registry.hpp"

using namespace minimal;
using namespace minimal::testing;
using plugins::LoadStatus;
using plugins::PluginCandidate;
using plugins::Provenance;
using tools::PermissionMode;
using tools::PermissionRunMode;

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

TEST_CASE("the loader dlsyms the mention-resolver seam into a real registry",
          "[plugin_loader][mention]") {
    tools::ToolRegistry tool_registry;
    llm::ProviderFactoryRegistry provider_registry;
    agent::MentionResolverRegistry resolvers(common::DefaultLogger());
    plugins::PluginLoader loader(tool_registry, provider_registry, nlohmann::json::object(),
                                 /*observers=*/nullptr, /*commands=*/nullptr,
                                 /*auth_providers=*/nullptr, &resolvers);
    const auto records = loader.LoadAll({Enabled("fixture-good")});

    REQUIRE(records.size() == 1);
    CHECK(records.front().registered_mention_resolver);
    CHECK(resolvers.has_override());

    // The override reached Active(), not just the record: a plugin-registered
    // strategy must actually serve SuggestPaths, which is the entire reason the
    // seam is exclusive rather than additive.
    CHECK(resolvers.Active().Suggest("", {}, 50) == std::vector<std::string>{"fixture-resolved"});
}

TEST_CASE("a loader without the registry leaves the seam unoffered", "[plugin_loader][mention]") {
    Harness h;  // default-constructed: no mention-resolver registry attached
    const auto records = h.loader.LoadAll({Enabled("fixture-good")});

    REQUIRE(records.size() == 1);
    CHECK(records.front().status == LoadStatus::Loaded);
    // Null registry means the symbol is simply not offered; the plugin treats
    // every registration hook as optional, so the load still succeeds.
    CHECK_FALSE(records.front().registered_mention_resolver);
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

// --- Aliases ------------------------------------------------------------------
//
// fixture-good and fixture-collision both register a tool named "fixture_tool" --
// the exact shape of two plugins claiming `bash`. The execute() result text
// distinguishes which implementation is alive.

namespace {

struct AliasHarness {
    tools::ToolRegistry tool_registry;
    llm::ProviderFactoryRegistry provider_registry;
    std::map<std::string, std::string> aliases;

    // ApplyAliases runs after ALL registration (plugins + core built-ins), which
    // is what makes a shadowed first registrant recoverable when the second one
    // is the aliased side -- so it happens here, not per plugin.
    std::vector<plugins::PluginRecord> LoadBoth() {
        auto records = plugins::PluginLoader(tool_registry, provider_registry)
                           .LoadAll({Enabled("fixture-good"), Enabled("fixture-collision")});
        tool_registry.ApplyAliases(aliases);
        return records;
    }
};

}  // namespace

TEST_CASE("an aliased tool is registered under its surface name", "[plugin_loader][alias]") {
    tools::ToolRegistry tool_registry;
    llm::ProviderFactoryRegistry provider_registry;
    plugins::PluginLoader loader{tool_registry, provider_registry};

    const auto records = loader.LoadAll({Enabled("fixture-good")});
    tool_registry.ApplyAliases({{"fixture-good.fixture_tool", "ftool"}});
    REQUIRE(records.size() == 1);
    CHECK(records.front().status == LoadStatus::Loaded);
    // The record still reports the ORIGINAL name: collision detection and
    // permission keys are computed from "who provided what", which a rename
    // does not change.
    CHECK(records.front().registered_tools == std::vector<std::string>{"fixture_tool"});

    CHECK(tool_registry.Find("ftool") != nullptr);
    CHECK(tool_registry.Find("fixture_tool") == nullptr);
    REQUIRE(tool_registry.Find("ftool"));
    CHECK(tool_registry.Find("ftool")->spec().name == "ftool");
    // The definition advertises the surface name too: the model calls what it sees.
    CHECK(tool_registry.Find("ftool")->spec().definition.name == "ftool");

    // Attribution is untouched by the rename -- one entry, still the original name.
    const auto& attributions = tool_registry.attributions();
    REQUIRE(attributions.size() == 1);
    CHECK(attributions.front().plugin_name == "fixture-good");
    CHECK(attributions.front().tool_name == "fixture_tool");
}

TEST_CASE("an alias resolves a collision: both implementations survive",
          "[plugin_loader][alias][collision]") {
    AliasHarness h;
    h.aliases = {{"fixture-collision.fixture_tool", "ftool2"}};
    const auto records = h.LoadBoth();
    REQUIRE(records.size() == 2);
    CHECK(records[0].status == LoadStatus::Loaded);
    CHECK(records[1].status == LoadStatus::Loaded);

    // Without application at registration, insert_or_assign would have silently
    // dropped one of the two -- this is the soundness hole aliasing exists to close.
    REQUIRE(h.tool_registry.Find("fixture_tool") != nullptr);
    REQUIRE(h.tool_registry.Find("ftool2") != nullptr);

    tools::ToolContext ctx(common::DefaultLogger());
    const auto good = h.tool_registry.Find("fixture_tool")->execute({}, ctx);
    const auto aliased = h.tool_registry.Find("ftool2")->execute({}, ctx);
    CHECK(good.content_text == "fixture ok");
    CHECK(aliased.content_text == "collision ok");

    // Name resolution agrees with the registry: no fatal collision, and the
    // shared original name makes BOTH permission keys qualified.
    const auto resolution = plugins::ResolveRegistrationNames(h.tool_registry, h.aliases);
    CHECK(resolution.result.fatal_collisions.empty());
    CHECK(resolution.result.dangling_aliases.empty());
    h.tool_registry.SetPermissionKeys(resolution.permission_keys);
    CHECK(std::string(h.tool_registry.PermissionKeyFor("fixture_tool")) ==
          "fixture-good.fixture_tool");
    CHECK(std::string(h.tool_registry.PermissionKeyFor("ftool2")) ==
          "fixture-collision.fixture_tool");

    // allowed_tools addresses the permission key, never the surface name.
    const auto only_aliased = h.tool_registry.DefinitionsFor({"fixture-collision.fixture_tool"});
    REQUIRE(only_aliased.size() == 1);
    CHECK(only_aliased[0].name == "ftool2");
    const auto only_good = h.tool_registry.DefinitionsFor({"fixture-good.fixture_tool"});
    REQUIRE(only_good.size() == 1);
    CHECK(only_good[0].name == "fixture_tool");
}

TEST_CASE("two plugins claiming one name without an alias collide", "[plugin_loader][collision]") {
    AliasHarness h;
    const auto records = h.LoadBoth();
    REQUIRE(records.size() == 2);

    const auto resolution = plugins::ResolveRegistrationNames(h.tool_registry, h.aliases);
    REQUIRE(resolution.result.fatal_collisions.size() == 1);
    CHECK(resolution.result.fatal_collisions[0] == "fixture_tool");

    // Both sides are nameable from the attributions: the fatal diagnostic must
    // say WHICH two plugins collided, not merely that something did.
    const auto& attributions = h.tool_registry.attributions();
    REQUIRE(attributions.size() == 2);
    CHECK(attributions[0].plugin_name == "fixture-good");
    CHECK(attributions[1].plugin_name == "fixture-collision");
}

TEST_CASE("an alias naming nothing registered is dangling, not fatal", "[plugin_loader][alias]") {
    tools::ToolRegistry tool_registry;
    llm::ProviderFactoryRegistry provider_registry;
    const std::map<std::string, std::string> aliases = {
        {"fixture-good.no_such_tool", "x"}, {"some-other-plugin.tool", "y"}};

    plugins::PluginLoader loader{tool_registry, provider_registry};
    loader.LoadAll({Enabled("fixture-good")});
    tool_registry.ApplyAliases(aliases);

    const auto resolution = plugins::ResolveRegistrationNames(tool_registry, aliases);
    CHECK(resolution.result.fatal_collisions.empty());
    REQUIRE(resolution.result.dangling_aliases.size() == 2);
    // Sorted: startup diagnostics must be stable run to run.
    CHECK(resolution.result.dangling_aliases[0] == "fixture-good.no_such_tool");
    CHECK(resolution.result.dangling_aliases[1] == "some-other-plugin.tool");
}

TEST_CASE("per_tool_overrides govern an aliased tool through its permission key",
          "[plugin_loader][alias][permission]") {
    AliasHarness h;
    h.aliases = {{"fixture-collision.fixture_tool", "ftool2"}};
    h.LoadBoth();
    const auto resolution = plugins::ResolveRegistrationNames(h.tool_registry, h.aliases);
    h.tool_registry.SetPermissionKeys(resolution.permission_keys);

    // Deny ONLY the aliased tool's qualified key. A bare "fixture_tool" override
    // would be ambiguous (two plugins provide that original name) and must not
    // govern either one.
    tools::PermissionPolicy policy = AllowAllPolicy();
    policy.per_tool_overrides["fixture-collision.fixture_tool"] = PermissionMode::AlwaysDeny;

    UnreachablePermissionChecker checker;  // a prompt would mean the key was missed
    tools::ToolExecutor executor(h.tool_registry, checker, policy, {},
                                 PermissionRunMode::Normal);
    tools::ToolContext ctx(common::DefaultLogger());

    const auto denied = executor.Run("t1", "ftool2", nlohmann::json::object(), ctx);
    CHECK(denied.is_error);
    CHECK(!checker.called);  // denied by policy, never reached the checker

    const auto allowed = executor.Run("t2", "fixture_tool", nlohmann::json::object(), ctx);
    CHECK_FALSE(allowed.is_error);
    CHECK(!checker.called);
}

TEST_CASE("a reserved core tool name collides with a plugin claiming it",
          "[plugin_loader][collision][invariant]") {
    // spawn_subagent is registered by CORE at serve stage -- after the
    // startup collision check has run. Unreserved, a plugin tool claiming the
    // name silently shadowed (or was shadowed by) the core tool via
    // insert_or_assign: the exact failure mode the fatal-collision rule
    // exists to prevent, reopened through registration order alone. The
    // reservation feeds the name into the check as the built-in it is.
    tools::ToolRegistry registry;
    registry.SetAttribution("bash_over_ssh");
    registry.Register(std::make_unique<testing::FakeTool>("spawn_subagent",
                                                          tools::ToolCategory::Execute));
    registry.ClearAttribution();

    const auto resolution =
        plugins::ResolveRegistrationNames(registry, {}, nullptr,
                                          /*core_tool_names=*/{"spawn_subagent"});
    REQUIRE(resolution.result.fatal_collisions.size() == 1);
    CHECK(resolution.result.fatal_collisions[0] == "spawn_subagent");

    // The proof the reservation is load-bearing: the same registration WITHOUT
    // it produced no collision, which is the hole.
    const auto unreserved = plugins::ResolveRegistrationNames(registry, {});
    CHECK(unreserved.result.fatal_collisions.empty());
}

TEST_CASE("a late-registered duplicate does not silently shadow the incumbent",
          "[plugin_loader][collision][invariant]") {
    // ToolRegistry::Register once took the insert_or_assign branch after alias
    // application, so any post-alias duplicate replaced the incumbent with no
    // diagnostic and the loser was destroyed. First-wins everywhere: the
    // duplicate is parked as shadowed and the incumbent stays served.
    tools::ToolRegistry registry;
    auto first = std::make_unique<testing::FakeTool>("bash", tools::ToolCategory::Execute,
                                                     "first");
    auto* first_ptr = first.get();
    auto second = std::make_unique<testing::FakeTool>("bash", tools::ToolCategory::Execute,
                                                      "second");
    registry.Register(std::move(first));
    registry.ApplyAliases({});  // closes the pre-alias path
    registry.Register(std::move(second));

    tools::ToolContext ctx(common::DefaultLogger());
    const auto served = registry.Find("bash")->execute({}, ctx);
    CHECK(served.content_text == "first");  // the incumbent, not the latecomer
    CHECK(first_ptr->calls.size() == 1);
}
