// typesafe-advisor -- auto-mode as a track-J permission advisor (docs/
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
#include "minimal/common/logger.hpp"
#include "minimal/net/http_client.hpp"
#include "minimal/plugins/plugin_abi.h"
#include "minimal/plugins/service_registry.hpp"
#include "minimal/tools/permission_advisor.hpp"

#include "advisor.hpp"
#include "settings.hpp"

extern "C" {

MINIMAL_PLUGIN_ABI_SYMBOLS("typesafe-advisor")

void minimal_plugin_register_permission_advisor(
    minimal::tools::PermissionAdvisorRegistry& registry,
    const minimal::plugins::PluginRegistrationContext& context) {
    const auto settings = minimal::typesafe_advisor::ParseSettings(context.config);

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
                      .Bind(settings.api_key, settings.EffectiveEndpoint());
    } catch (const std::exception& error) {
        minimal::common::DefaultLogger().error(
            "typesafe-advisor: api_key {}; the advisor will not register", error.what());
        return;
    }

    registry.Register(std::make_shared<minimal::typesafe_advisor::TypesafeAdvisor>(
        settings, std::move(api_key), minimal::net::HttpClient{}));
}

}  // extern "C"
