#pragma once

// Every config file the server reads at startup, loaded once. Loading only
// parses; it never judges cross-file consistency -- that is the startup
// sanity check's job, against the loaded bundle. A file that fails to load
// leaves its member default-constructed, its `*_loaded` flag false, and a
// problem naming it.

#include <filesystem>
#include <string>
#include <vector>

#include "minimal/config/hooks_config.hpp"
#include "minimal/config/permissions_config.hpp"
#include "minimal/config/providers_config.hpp"
#include "minimal/config/settings_config.hpp"

namespace minimal::config {

enum class CheckSeverity { Fatal, Warning };

// One startup diagnostic. Fatal aborts startup; Warning degrades and goes on.
struct CheckResult {
    CheckSeverity severity;
    std::string check_name;  // stable identifier, e.g. "usable_provider"
    std::string message;     // human-readable; names the offending file/entry/plugin
};

struct ConfigBundle {
    SettingsConfig settings;
    bool settings_loaded = false;
    PermissionsConfig permissions;
    bool permissions_loaded = false;
    ProvidersConfig providers;
    bool providers_loaded = false;
    HooksConfig hooks;
    // What went wrong loading, in file order. settings/permissions/providers/
    // hooks failures are Fatal (serving depends on them, or -- hooks -- a
    // silently dropped guard would be the dangerous outcome); a malformed
    // plugins.json degrades with a Warning. (mcps.json and channels.json
    // belong to the MCP plugin, which loads and validates them itself.)
    std::vector<CheckResult> problems;
};

// Loads every file under `config_dir`. Never throws for a file's content.
// `variant_params` resolves a providers.json list-form variant recipe at
// parse time (see VariantParametersFor).
ConfigBundle LoadConfigBundle(const std::filesystem::path& config_dir,
                              const VariantParamsHook& variant_params);

}  // namespace minimal::config
