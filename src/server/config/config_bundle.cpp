#include "minimal/config/config_bundle.hpp"

#include <exception>
#include <string>

#include <nlohmann/json.hpp>

#include "minimal/common/read_file.hpp"

namespace minimal::config {
namespace {

CheckResult Fatal(const std::string& check, const std::string& message) {
    return {CheckSeverity::Fatal, check, message};
}

CheckResult Warning(const std::string& check, const std::string& message) {
    return {CheckSeverity::Warning, check, message};
}

}  // namespace

ConfigBundle LoadConfigBundle(const std::filesystem::path& dir,
                              const VariantParamsHook& variant_params,
                              const ProviderConfigExtensions* provider_extensions) {
    ConfigBundle bundle;
    auto& results = bundle.problems;

    try {
        bundle.settings = LoadSettingsConfig(dir / "settings.json");
        bundle.settings_loaded = true;
    } catch (const std::exception& e) {
        // Serving requires this file: the preset registry and the session
        // service both consume it, so absence is fatal, not degraded.
        results.push_back(Fatal("config_parse", std::string("settings.json: ") + e.what()));
    }

    try {
        bundle.permissions = LoadPermissionsConfig(dir / "permissions.json");
        for (const auto& key : bundle.permissions.unknown_keys) {
            results.push_back(Warning("config_parse", "permissions.json: unknown key '" + key +
                                                          "' is ignored (typo?)"));
        }
    } catch (const std::exception& e) {
        results.push_back(Fatal("config_parse", std::string("permissions.json: ") + e.what()));
    }

    try {
        bundle.providers =
            LoadProvidersConfig(dir / "providers.json", variant_params, provider_extensions);
        bundle.providers_loaded = true;
    } catch (const std::exception& e) {
        results.push_back(Fatal("config_parse", std::string("providers.json: ") + e.what()));
    }

    try {
        bundle.hooks = LoadHooksConfig(dir / "hooks.json");
    } catch (const std::exception& e) {
        // Same severity as permissions.json: a hook action is shell code the
        // user opted into as a guardrail, and a parse error that silently
        // disabled the file's guard hooks would be the dangerous outcome.
        // Fatal names the problem; the user fixes or deletes the file.
        results.push_back(Fatal("config_parse", std::string("hooks.json: ") + e.what()));
    }

    // plugins.json degrades to empty BY DESIGN -- but a silently degraded file
    // is exactly what startup must make loud. An oversize or unreadable file
    // throws and is reported like the others.
    try {
        const auto plugins_text = common::ReadFileOrEmpty(dir / "plugins.json");
        if (!plugins_text.empty()) {
            const auto probe =
                nlohmann::json::parse(plugins_text, nullptr, /*allow_exceptions=*/false);
            if (probe.is_discarded() || !probe.is_object()) {
                results.push_back(
                    Warning("config_parse", "plugins.json is malformed; treating it as empty "
                                            "(discovered plugins keep their provenance defaults)"));
            }
        }
    } catch (const std::exception& e) {
        results.push_back(Fatal("config_parse", std::string("plugins.json: ") + e.what()));
    }

    return bundle;
}

}  // namespace minimal::config
