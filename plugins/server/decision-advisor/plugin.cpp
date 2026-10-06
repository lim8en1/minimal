// decision-advisor -- auto-mode as a track-J permission advisor (docs/
// EXTENSIBILITY.md §3): Jev answers the Ask-gated tool calls the substrate
// resolved to "ask a human"; abstentions (review choice, below-threshold
// probabilities, HTTP trouble, cancel) fall through to the wire checker.
//
// Ships DISABLED (DefaultsDisabled in plugin_discovery.cpp): it sends tool
// input to an external endpoint and auto-answers permission prompts, so
// enabling it is the operator's informed decision -- one plugins.json edit
// plus one permissions.json "advisor" field.
//
// The substrate is untouched and untrusting: the advisor can only NARROW what
// reaches a person. AllowOnly short-circuits before the advisor exists (no
// approvals smuggled into headless runs), and ToolExecutor re-filters every
// granted key against the disclosure, so a hostile or hallucinating judge
// mints nothing.

#include <cstdlib>
#include <memory>
#include <utility>

#include "minimal/config/secrets.hpp"
#include "minimal/common/config_paths.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/net/http_client.hpp"
#include "minimal/common/plugin_config_label.hpp"
#include "minimal/common/plugin_config_schema.hpp"
#include "minimal/plugins/plugin_abi.h"
#include "minimal/plugins/service_registry.hpp"
#include "minimal/tools/permission_advisor.hpp"

#include "advisor.hpp"
#include "settings.hpp"

MINIMAL_PLUGIN_CONFIG_SCHEMA(
    minimal::common::PluginConfigLabel(minimal::common::kPluginConfigDirName, "decision-advisor"),
    [](const nlohmann::json& config) {
        (void)minimal::decision_advisor::ParseSettings(
            minimal::decision_advisor::LoadBundledSettings(
                minimal::common::ConfigDir() / minimal::decision_advisor::kBundledSettingsFile),
            config);
    })

extern "C" {

MINIMAL_PLUGIN_ABI_SYMBOLS("decision-advisor")

void minimal_plugin_register_permission_advisor(
    minimal::tools::PermissionAdvisorRegistry& registry,
    const minimal::plugins::PluginRegistrationContext& context) {
    // Every value comes from the seeded bundled file under this entry's
    // overrides; a missing file or field throws, which keeps the plugin from
    // loading with the reason named -- there is no built-in fallback.
    const auto settings = minimal::decision_advisor::ParseSettings(
        minimal::decision_advisor::LoadBundledSettings(
            minimal::common::ConfigDir() / minimal::decision_advisor::kBundledSettingsFile),
        context.config);

    // Bound ONCE, here: a reference that can never resolve (an unset env
    // var) is a loud, contained registration failure (the plugin loads
    // degraded, the sanity check warns that the configured advisor resolves
    // to nothing), not a silent 401 on the first gated tool call hours later.
    // core.secrets is core-provided before any plugin loads.
    static const minimal::config::SecretResolver kDefaultSecrets;
    const auto* found =
        context.services != nullptr
            ? static_cast<const minimal::config::SecretResolver*>(
                  context.services->Find(minimal::config::kSecretsServiceName))
            : nullptr;
    minimal::common::SecretHandle api_key;
    try {
        api_key = (found != nullptr ? *found : kDefaultSecrets)
                      .Bind(settings.api_key, settings.endpoint);
    } catch (const std::exception& error) {
        minimal::common::DefaultLogger().error(
            "decision-advisor: api_key {}; the advisor will not register", error.what());
        return;
    }

    registry.Register(std::make_shared<minimal::decision_advisor::DecisionAdvisor>(
        settings, std::move(api_key), minimal::net::HttpClient{}));
}

}  // extern "C"
