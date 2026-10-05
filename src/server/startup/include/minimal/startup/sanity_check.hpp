#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "minimal/agent/agent_preset_frontmatter.hpp"
#include "minimal/config/config_bundle.hpp"
#include "minimal/agent/host_registries.hpp"
#include "minimal/plugins/name_resolution.hpp"
#include "minimal/plugins/plugin_discovery.hpp"
#include "minimal/plugins/plugin_loader.hpp"
#include "minimal/session/session_database.hpp"

namespace minimal::startup {

using config::CheckResult;
using config::CheckSeverity;

// Everything the checks need beyond the loaded config, captured at the
// stage-3 point in main(): plugins already loaded (records carry every load
// outcome), tools already registered and alias-resolved.
struct SanityCheckInputs {
    std::filesystem::path config_dir;
    const std::vector<plugins::PluginRecord>& plugin_records;
    plugins::PluginConfig plugin_config;  // aliases, enablement state
    // A VALUE, not a reference: it is computed once (by ResolveRegistrationNames) and
    // its owner must not have to outlive the inputs struct.
    plugins::RegistrationNames name_resolution;
    // tools and providers are required. Every other registry is optional:
    // absent means the checks that consult it degrade to structural ones (no
    // advisor can resolve, no discover-entry can resolve, hooks.json checked
    // without the registered points, compaction strategies checked against
    // core's built-ins only). All are consulted STRUCTURALLY -- which kinds
    // and names registered -- never by calling into a plugin.
    plugins::HostRegistries registries;
};

// Runs after config and plugin load, before serving. A linter, not a fail-fast
// validator: RunAll() surfaces EVERY problem found, so a misconfigured setup
// yields one complete diagnostic instead of a fix-one-rerun-find-the-next loop.
// A cold start with nothing configured is just more failing checks with clear
// reasons -- there is no onboarding wizard. Validates only: the config comes
// in already loaded (config::LoadConfigBundle), and its load problems are
// reported by whoever loaded it.
class StartupSanityCheck {
public:
    StartupSanityCheck(const config::ConfigBundle& bundle, SanityCheckInputs inputs);

    std::vector<CheckResult> RunAll();

private:
    std::vector<CheckResult> check_providers();
    std::vector<CheckResult> check_presets();
    std::vector<CheckResult> check_plugin_warnings();
    std::vector<CheckResult> check_directories();
    std::vector<CheckResult> check_plugins();
    std::vector<CheckResult> check_name_collisions();
    std::vector<CheckResult> check_naming_model();
    std::vector<CheckResult> check_advisor();
    std::vector<CheckResult> check_subagent_limit();
    std::vector<CheckResult> check_hooks();
    std::vector<CheckResult> check_compaction_strategy();
    std::vector<CheckResult> check_compaction_keep_recent();
    std::vector<CheckResult> check_small_model();
    std::vector<CheckResult> check_default_subagent_model();

    const config::ConfigBundle& bundle_;
    SanityCheckInputs inputs_;

    // agents/*.md parsed eagerly -- AgentPresetRegistry loads them lazily, which
    // is exactly why a broken preset stayed silent until the first session.
    std::unordered_map<std::string, agent::AgentPresetFrontmatter> presets_;
};

// Opens (and migrates) the session database settings.json names -- the
// default path when settings did not load. A database this binary is too old
// to understand is refused: running on would silently drop every transcript.
// Null, with a Fatal appended to `problems`, when it cannot be opened.
std::unique_ptr<session::SessionDatabase> OpenSessionDatabase(const config::ConfigBundle& bundle,
                                                              std::vector<CheckResult>& problems);

}  // namespace minimal::startup
