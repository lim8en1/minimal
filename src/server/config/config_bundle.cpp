#include "minimal/config/config_bundle.hpp"

#include <exception>
#include <string>

namespace minimal::config {
namespace {

CheckResult Fatal(const std::string& check, const std::string& message) {
    return {CheckSeverity::Fatal, check, message};
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
        results.push_back(Fatal("config_parse", e.what()));
    }

    try {
        bundle.permissions = LoadPermissionsConfig(dir / "permissions.json");
    } catch (const std::exception& e) {
        results.push_back(Fatal("config_parse", e.what()));
    }

    try {
        bundle.providers =
            LoadProvidersConfig(dir / "providers.json", variant_params, provider_extensions);
        bundle.providers_loaded = true;
    } catch (const std::exception& e) {
        results.push_back(Fatal("config_parse", e.what()));
    }

    try {
        bundle.hooks = LoadHooksConfig(dir / "hooks.json");
    } catch (const std::exception& e) {
        // Same severity as permissions.json: a hook action is shell code the
        // user opted into as a guardrail, and a parse error that silently
        // disabled the file's guard hooks would be the dangerous outcome.
        // Fatal names the problem; the user fixes or deletes the file.
        results.push_back(Fatal("config_parse", e.what()));
    }

    return bundle;
}

}  // namespace minimal::config
