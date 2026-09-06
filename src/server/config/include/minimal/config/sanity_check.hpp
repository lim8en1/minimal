#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "minimal/agent/agent_preset_frontmatter.hpp"
#include "minimal/agent/compaction_strategy.hpp"
#include "minimal/config/channels_config.hpp"
#include "minimal/config/hooks_config.hpp"
#include "minimal/config/mcps_config.hpp"
#include "minimal/config/permissions_config.hpp"
#include "minimal/config/providers_config.hpp"
#include "minimal/config/settings_config.hpp"
#include "minimal/config/skill_registry.hpp"
#include "minimal/llm/provider_factory_registry.hpp"
#include "minimal/commands/command_registry.hpp"
#include "minimal/plugins/name_resolution.hpp"
#include "minimal/plugins/plugin_discovery.hpp"
#include "minimal/plugins/plugin_loader.hpp"
#include "minimal/session/session_database.hpp"
#include "minimal/tools/permission_advisor.hpp"
#include "minimal/tools/tool_registry.hpp"

namespace minimal::hooks {
class HookDispatcher;  // borrowed via SanityCheckInputs::hook_dispatcher
}  // namespace minimal::hooks

namespace minimal::config {

enum class CheckSeverity { Fatal, Warning };

struct CheckResult {
    CheckSeverity severity;
    std::string check_name;  // stable identifier, e.g. "usable_provider"
    std::string message;     // human-readable; names the offending file/entry/plugin
};

// Everything the checks need, captured at the stage-3 point in main(): config
// already parsed to the extent it parses, plugins already loaded (records carry
// every load outcome), tools already registered and alias-resolved.
struct SanityCheckInputs {
    std::filesystem::path config_dir;
    const std::vector<plugins::PluginRecord>& plugin_records;
    const tools::ToolRegistry& tool_registry;  // attributions: both sides of a collision
    const llm::ProviderFactoryRegistry& provider_factories;
    const SkillRegistry& skills;
    plugins::PluginConfig plugin_config;  // aliases, enablement state
    // A VALUE, not a reference: it is computed once (by ResolveRegistrationNames) and
    // its owner must not have to outlive the inputs struct.
    plugins::RegistrationNames name_resolution;
    // Optional: a service built without commands has none to check. Borrowed --
    // it outlives the check, which runs before serving.
    const commands::CommandRegistry* command_registry = nullptr;
    // Optional: absent means no advisor can resolve, which only matters when
    // permissions.json names one.
    const tools::PermissionAdvisorRegistry* permission_advisors = nullptr;
    // Optional: absent means no services were offered to plugins, so no service
    // collision can exist. Borrowed -- it outlives the check.
    const plugins::ServiceRegistry* service_registry = nullptr;
    // Optional: absent means no discover-entry can resolve, which only matters
    // when providers.json sets "discover": true. Borrowed -- it outlives the
    // check. Deliberately consulted STRUCTURALLY only (which kinds registered,
    // never a live List()): discovery is fetched lazily at first resolve,
    // never at startup.
    const llm::ModelCatalogRegistry* model_catalogs = nullptr;
    // Optional: absent means memory.compaction_strategy can only be checked
    // against core's own built-in name ("summarize"), not against whatever
    // plugins additionally registered. Borrowed -- it outlives the check.
    const agent::CompactionStrategyRegistry* compaction_strategies = nullptr;
    // Optional: absent means hooks.json actions can only be validated
    // structurally, not against the registered points. Borrowed -- it outlives
    // the check. Points are registered at load (before this stage), so the
    // point-name and filterable-field checks are structural too: no emission
    // is ever fired from a sanity check.
    const hooks::HookDispatcher* hook_dispatcher = nullptr;
};

// Runs after config and plugin load, before serving. A linter, not a fail-fast
// validator: RunAll() surfaces EVERY problem found, so a misconfigured setup
// yields one complete diagnostic instead of a fix-one-rerun-find-the-next loop.
// A cold start with nothing configured is just more failing checks with clear
// reasons -- there is no onboarding wizard.
class StartupSanityCheck {
public:
    explicit StartupSanityCheck(SanityCheckInputs inputs);

    std::vector<CheckResult> RunAll();

    // Valid only when RunAll() produced no Fatal results: the serve stage takes
    // these rather than re-loading (and re-throwing) the same files.
    const SettingsConfig& settings() const { return settings_; }
    const PermissionsConfig& permissions() const { return permissions_; }
    const ProvidersConfig& providers() const { return providers_; }
    const McpsConfig& mcps() const { return mcps_; }
    const ChannelsConfig& channels() const { return channels_; }
    // Valid after RunAll() even when the load failed -- the value is then the
    // default (no hooks), and the config_parse Fatal is what names the problem.
    const HooksConfig& hooks() const { return hooks_; }
    std::unique_ptr<session::SessionDatabase> take_session_db() { return std::move(session_db_); }

private:
    std::vector<CheckResult> check_config_files();
    std::vector<CheckResult> check_providers();
    std::vector<CheckResult> check_presets();
    std::vector<CheckResult> check_mcp_servers();
    std::vector<CheckResult> check_channels();
    std::vector<CheckResult> check_directories();
    std::vector<CheckResult> check_session_db();
    std::vector<CheckResult> check_plugins();
    std::vector<CheckResult> check_name_collisions();
    std::vector<CheckResult> check_naming_model();
    std::vector<CheckResult> check_advisor();
    std::vector<CheckResult> check_subagent_limit();
    std::vector<CheckResult> check_hooks();
    std::vector<CheckResult> check_compaction_strategy();
    std::vector<CheckResult> check_small_model();

    SanityCheckInputs inputs_;

    // Populated by the checks as they run; the accessors above expose them.
    SettingsConfig settings_;
    PermissionsConfig permissions_;
    ProvidersConfig providers_;
    McpsConfig mcps_;
    ChannelsConfig channels_;
    HooksConfig hooks_;
    std::unique_ptr<session::SessionDatabase> session_db_;
    bool settings_loaded_ = false;
    bool providers_loaded_ = false;

    // agents/*.md parsed eagerly -- AgentPresetRegistry loads them lazily, which
    // is exactly why a broken preset stayed silent until the first session.
    std::unordered_map<std::string, agent::AgentPresetFrontmatter> presets_;
};

}  // namespace minimal::config
