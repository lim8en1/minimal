#include <catch2/catch_test_macros.hpp>

#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "fakes.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/hooks/hook_dispatcher.hpp"
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
    llm::ModelCatalogRegistry model_catalog_registry;
    plugins::ServiceRegistry service_registry;
    hooks::HookDispatcher hook_dispatcher{common::DefaultLogger()};
    plugins::PluginLoader loader{tool_registry, provider_registry, nlohmann::json::object(),
                                 nullptr, nullptr, nullptr, nullptr, &model_catalog_registry,
                                 &service_registry, nullptr, {}, &hook_dispatcher};
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
    CHECK(record.registered_model_catalog_kinds == std::vector<std::string>{"fixture"});

    // The registrations reached the real registries, not just the record.
    CHECK(h.tool_registry.Find("fixture_tool") != nullptr);
    CHECK(h.provider_registry.Has("fixture"));
    CHECK(h.provider_registry.Create("fixture", {}) != nullptr);
    // ... including the catalog (track N): it answers under the same kind.
    const auto* catalog = h.model_catalog_registry.Find("fixture");
    REQUIRE(catalog != nullptr);
    const auto listed = catalog->List({});
    REQUIRE(listed.size() == 1);
    CHECK(listed[0].id == "fixture-model");
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
    // aliases at the top level; both cannot hold.
    const std::string text = R"({
      "plugins": [
        {"file": "tools-fs.so", "provenance": "bundled", "enabled": true},
        {"file": "custom.so", "provenance": "user", "enabled": false}
      ],
      "aliases": {"bash_over_ssh.bash": "ssh_bash"}
    })";

    const auto config = plugins::ParsePluginConfig(text);
    REQUIRE(config.plugins.size() == 2);
    CHECK(config.plugins[0].file == "tools-fs.so");
    CHECK(config.plugins[0].provenance == Provenance::Bundled);
    CHECK(config.plugins[0].enabled);
    CHECK(config.plugins[1].provenance == Provenance::User);
    CHECK_FALSE(config.plugins[1].enabled);
    REQUIRE(config.aliases.count("bash_over_ssh.bash") == 1);
    CHECK(config.aliases.at("bash_over_ssh.bash") == "ssh_bash");

    const auto reparsed = plugins::ParsePluginConfig(plugins::SerializePluginConfig(config));
    REQUIRE(reparsed.plugins.size() == 2);
    CHECK(reparsed.plugins[0].file == config.plugins[0].file);
    CHECK(reparsed.aliases == config.aliases);
}

TEST_CASE("malformed plugins.json degrades to empty rather than throwing",
          "[plugin_config]") {
    CHECK(plugins::ParsePluginConfig("{ not json").plugins.empty());
    CHECK(plugins::ParsePluginConfig("[]").plugins.empty());   // wrong shape
    CHECK(plugins::ParsePluginConfig("").plugins.empty());
}

TEST_CASE("the legacy command_aliases key is ignored, not applied", "[plugin_config]") {
    // Renamed to "aliases" with NO fallback: a file from before the rename
    // must not have its aliases silently apply under a key the docs no longer
    // name -- and must not have them silently NOT apply either. The key is
    // preserved verbatim (it round-trips through `preserved`), parsed into
    // nothing, and the rename is warned about by the parser. This test pins
    // the first two properties; the warning is a log line, not observable
    // here.
    const std::string text = R"({"plugins":[],"command_aliases":{"p.cmd":"alias"}})";
    const auto config = plugins::ParsePluginConfig(text);
    CHECK(config.aliases.empty());

    const auto round_tripped = nlohmann::json::parse(plugins::SerializePluginConfig(config));
    // Preserved, so the operator's file is not edited behind their back --
    // and the warning has something to point at on the next boot too.
    CHECK(round_tripped.at("command_aliases").at("p.cmd") == "alias");
    CHECK_FALSE(round_tripped.contains("aliases"));
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

// --- Per-plugin config (track K) ----------------------------------------------
//
// fixture-good and fixture-collision echo their config's "echo_value" through
// their tools, so config delivery is observable through a real dlopen rather
// than asserted on a struct the test compiled itself.

TEST_CASE("each plugin's registration context sees only its own config",
          "[plugin_loader][track_k]") {
    // The isolation property. Both plugins load in one process; if the loader
    // handed every context one shared config (or the wrong plugin's), the tool
    // outputs would cross.
    tools::ToolRegistry tool_registry;
    llm::ProviderFactoryRegistry provider_registry;
    plugins::PluginLoader loader{tool_registry, provider_registry};

    PluginCandidate good = Enabled("fixture-good");
    good.config = nlohmann::json{{"echo_value", "good config"}};
    PluginCandidate collision = Enabled("fixture-collision");
    collision.config = nlohmann::json{{"echo_value", "collision config"}};

    const auto records = loader.LoadAll({good, collision});
    REQUIRE(records.size() == 2);
    CHECK(records[0].status == LoadStatus::Loaded);
    CHECK(records[1].status == LoadStatus::Loaded);

    // Both register "fixture_tool"; first-wins parks the second. Alias it so
    // both implementations are reachable and their configs observable.
    tool_registry.ApplyAliases({{"fixture-collision.fixture_tool", "ftool2"}});

    tools::ToolContext ctx(common::DefaultLogger());
    CHECK(tool_registry.Find("fixture_tool")->execute({}, ctx).content_text ==
          "good config");
    REQUIRE(tool_registry.Find("ftool2") != nullptr);
    CHECK(tool_registry.Find("ftool2")->execute({}, ctx).content_text ==
          "collision config");
}

TEST_CASE("a plugin entry with no config still loads with an empty context config",
          "[plugin_loader][track_k]") {
    // An absent config is a runtime condition, not an error: the tool falls
    // back to its default echo.
    Harness h;
    const auto records = h.loader.LoadAll({Enabled("fixture-good")});

    REQUIRE(records.size() == 1);
    CHECK(records.front().status == LoadStatus::Loaded);

    tools::ToolContext ctx(common::DefaultLogger());
    CHECK(h.tool_registry.Find("fixture_tool")->execute({}, ctx).content_text ==
          "fixture ok");
}

TEST_CASE("a plugin's hook points register before its tools, and land on the record",
          "[plugin_loader][track_o]") {
    Harness h;
    const auto records = h.loader.LoadAll({Enabled("fixture-hook")});

    REQUIRE(records.size() == 1);
    const auto& record = records.front();
    CHECK(record.status == LoadStatus::Loaded);
    CHECK(record.registered_hook_points == std::vector<std::string>{"fixture-hook.before_thing"});

    // The ordering guarantee the worked example (tools-bash) rests on: when the
    // plugin's tools registered, its HookPoint handle was already minted and
    // enabled. Exposed through the service channel because a dlopen'd plugin's
    // symbols are private.
    const auto* flag = static_cast<const bool*>(
        h.service_registry.Find("fixture.hook_captured_before_tools"));
    REQUIRE(flag != nullptr);
    CHECK(*flag);
}

TEST_CASE("config reaches the tool seam while tools_settings stays verbatim",
          "[plugin_loader][track_k]") {
    // The two channels are independent: config is per-plugin, tools_settings is
    // the whole verbatim `tools` object every tool plugin still receives.
    tools::ToolRegistry tool_registry;
    llm::ProviderFactoryRegistry provider_registry;
    const nlohmann::json tools_settings = {
        {"some_third_party_tool", nlohmann::json::object()}};
    plugins::PluginLoader loader{tool_registry, provider_registry, tools_settings};

    PluginCandidate good = Enabled("fixture-good");
    good.config = nlohmann::json{{"echo_value", "from config"}};
    REQUIRE(loader.LoadAll({good}).front().status == LoadStatus::Loaded);

    tools::ToolContext ctx(common::DefaultLogger());
    CHECK(tool_registry.Find("fixture_tool")->execute({}, ctx).content_text ==
          "from config");
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

TEST_CASE("a plugin whose registration throws is reported, not fatal to the process",
          "[plugin_loader][invariant]") {
    // The callbacks used to be invoked bare, so a throw propagated out of
    // LoadOne, out of LoadAll, into main() -- which has no handler -- and killed
    // the server with no diagnostic naming the plugin. Registration was the one
    // plugin entry point in the tree not wrapped; observers, commands and tool
    // execution all already were.
    Harness h;
    std::vector<plugins::PluginRecord> records;
    REQUIRE_NOTHROW(records = h.loader.LoadAll({Enabled("fixture-throws")}));

    REQUIRE(records.size() == 1);
    CHECK(records.front().status == LoadStatus::RegistrationThrew);
    CHECK(records.front().name == "fixture-throws");  // named, so it can be disabled
    CHECK(records.front().error.find("registration is broken") != std::string::npos);
}

TEST_CASE("a throwing plugin does not mis-attribute the next plugin's registrations",
          "[plugin_loader][invariant]") {
    // The attribution window is what lets a collision name both culprits. An
    // unwinding callback skipped ClearAttribution(), so every registration after
    // it -- from entirely unrelated plugins -- was filed under the plugin that
    // had already failed, and the collision diagnostic would then name the wrong
    // one.
    Harness h;
    const auto records = h.loader.LoadAll({Enabled("fixture-throws"), Enabled("fixture-good")});

    REQUIRE(records.size() == 2);
    CHECK(records[0].status == LoadStatus::RegistrationThrew);
    CHECK(records[1].status == LoadStatus::Loaded);

    for (const auto& attribution : h.tool_registry.attributions()) {
        if (attribution.tool_name == "fixture_tool") {
            CHECK(attribution.plugin_name == "fixture-good");
        }
    }
}

TEST_CASE("two plugins claiming one provider kind is a reported collision",
          "[plugin_loader][invariant]") {
    // The third registration namespace, and the one that had no rule. Register()
    // returned false on a duplicate and its own header said the caller would
    // report which two plugins collided -- the loader ignored the return, so the
    // loser was dropped in silence and which backend a session got depended on
    // plugin load order. Tools and commands are both fatal on collision; this
    // was exempt.
    llm::ProviderFactoryRegistry registry;
    const auto factory = [](const llm::ProviderConfig&) -> std::unique_ptr<llm::LlmProvider> {
        return nullptr;
    };

    registry.SetAttribution("plugin-a");
    CHECK(registry.Register("openai", factory));
    registry.ClearAttribution();

    registry.SetAttribution("plugin-b");
    CHECK_FALSE(registry.Register("openai", factory));  // first registration keeps the slot
    registry.ClearAttribution();

    REQUIRE(registry.collisions().size() == 1);
    CHECK(registry.collisions().front().kind == "openai");
    CHECK(registry.collisions().front().first_plugin == "plugin-a");
    CHECK(registry.collisions().front().second_plugin == "plugin-b");
}

TEST_CASE("plugins.json keeps keys the parser does not model", "[plugin_loader][config]") {
    // This file is rewritten on every start (discovery persists what it found),
    // and the serializer reconstructed the document from two known fields -- so
    // any other top-level key was silently deleted on the next boot, in a file
    // the user is explicitly invited to edit.
    const auto config = plugins::ParsePluginConfig(R"({
        "plugins": [{"file": "a.so", "provenance": "user", "enabled": true}],
        "aliases": {"p.cmd": "alias"},
        "notes": "kept by hand",
        "future_key": {"nested": 1}
    })");

    const auto round_tripped =
        nlohmann::json::parse(plugins::SerializePluginConfig(config));
    CHECK(round_tripped.at("notes") == "kept by hand");
    CHECK(round_tripped.at("future_key").at("nested") == 1);
    // ...and the modelled halves still serialize normally.
    REQUIRE(round_tripped.at("plugins").size() == 1);
    CHECK(round_tripped.at("plugins").at(0).at("file") == "a.so");
    CHECK(round_tripped.at("aliases").at("p.cmd") == "alias");
}

TEST_CASE("a plugin provides a service through the real loader", "[plugin_loader][service]") {
    Harness h;
    const auto records = h.loader.LoadAll({Enabled("fixture-service")});
    REQUIRE(records.size() == 1);
    CHECK(records.front().status == LoadStatus::Loaded);

    // The record names the services under the plugin's attribution. The
    // context_ok marker is provided only when PluginRegistrationContext::services
    // was the very registry dispatched into, which is the consumer channel a
    // plugin captures for later Find calls.
    CHECK(records.front().registered_services ==
          std::vector<std::string>{"fixture.service", "fixture.context_services_ok"});

    // ...and the registry holds both.
    CHECK(h.service_registry.Find("fixture.service") != nullptr);
    CHECK(h.service_registry.Find("fixture.context_services_ok") != nullptr);

    const auto& attributions = h.service_registry.attributions();
    REQUIRE(attributions.size() == 2);
    CHECK(attributions[0].plugin_name == "fixture-service");
    CHECK(attributions[0].service_name == "fixture.service");
}

TEST_CASE("a plugin offering no service seam loads cleanly", "[plugin_loader][service]") {
    // Symbol presence is the capability declaration -- register_services is
    // optional like every other hook.
    Harness h;
    const auto records = h.loader.LoadAll({Enabled("fixture-good")});
    REQUIRE(records.size() == 1);
    CHECK(records.front().registered_services.empty());
    CHECK(h.service_registry.attributions().empty());
}

TEST_CASE("two plugins providing one service name collide",
          "[plugin_loader][service][collision]") {
    Harness h;
    const auto records = h.loader.LoadAll({Enabled("fixture-service"), Enabled("fixture-service-b")});
    REQUIRE(records.size() == 2);
    CHECK(records[0].status == LoadStatus::Loaded);
    CHECK(records[1].status == LoadStatus::Loaded);

    const auto resolution = plugins::ResolveRegistrationNames(
        h.tool_registry, {}, nullptr, {}, &h.service_registry);
    REQUIRE(resolution.result.fatal_collisions.size() == 1);
    CHECK(resolution.result.fatal_collisions[0] == "fixture.service");

    // The diagnostic names both claimants -- that is WHY attributions are taken.
    // (fixture-service provides twice: the service itself plus the context_ok
    // marker; fixture-service-b provides once.)
    const auto& attributions = h.service_registry.attributions();
    REQUIRE(attributions.size() == 3);
    CHECK(attributions[0].plugin_name == "fixture-service");
    CHECK(attributions[0].service_name == "fixture.service");
    CHECK(attributions[2].plugin_name == "fixture-service-b");
    CHECK(attributions[2].service_name == "fixture.service");
}

TEST_CASE("a service name does not collide with an equal tool name",
          "[plugin_loader][service][collision]") {
    // Separate namespaces: a tool and a service may share a surface name, the
    // same independence commands already have from tools.
    Harness h;
    h.tool_registry.SetAttribution("fixture-service");
    h.tool_registry.Register(
        std::make_unique<testing::FakeTool>("fixture.service", tools::ToolCategory::ReadOnly));
    h.tool_registry.ClearAttribution();
    REQUIRE(h.loader.LoadAll({Enabled("fixture-service")}).front().status == LoadStatus::Loaded);

    const auto resolution = plugins::ResolveRegistrationNames(
        h.tool_registry, {}, nullptr, {}, &h.service_registry);
    CHECK(resolution.result.fatal_collisions.empty());
}

TEST_CASE("the service collision check is load-bearing",
          "[plugin_loader][service][collision][invariant]") {
    // Removing the NameKind::Service feed from ResolveRegistrationNames (the
    // "last-wins" world) must fail the collision test above. This test holds
    // the observable half: WITHOUT the feed, the same two plugins produce no
    // collision -- which is exactly the hole.
    Harness h;
    REQUIRE(h.loader.LoadAll({Enabled("fixture-service"), Enabled("fixture-service-b")}).size() ==
            2);

    const auto without_service_feed =
        plugins::ResolveRegistrationNames(h.tool_registry, {}, nullptr, {});
    CHECK(without_service_feed.result.fatal_collisions.empty());

    const auto with_service_feed =
        plugins::ResolveRegistrationNames(h.tool_registry, {}, nullptr, {}, &h.service_registry);
    CHECK_FALSE(with_service_feed.result.fatal_collisions.empty());
}
