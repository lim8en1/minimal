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

#include "minimal/common/env.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/net/http_client.hpp"
#include "minimal/plugins/plugin_abi.h"
#include "minimal/tools/permission_advisor.hpp"

#include "advisor.hpp"
#include "settings.hpp"

extern "C" {

MINIMAL_PLUGIN_ABI_SYMBOLS("typesafe-advisor")

void minimal_plugin_register_permission_advisor(
    minimal::tools::PermissionAdvisorRegistry& registry,
    const minimal::plugins::PluginRegistrationContext& context) {
    const auto settings = minimal::typesafe_advisor::ParseSettings(context.config);

    // Resolved ONCE, here: an env var that is unset at startup is a loud,
    // contained registration failure (the plugin loads degraded, the sanity
    // check warns that the configured advisor resolves to nothing), not a
    // silent 401 on the first gated tool call hours later.
    const auto api_key = minimal::common::LookupEnv(settings.api_key_env);
    if (!api_key) {
        minimal::common::DefaultLogger().error(
            "typesafe-advisor: environment variable {} is not set; the advisor will not register "
            "(set it, or point api_key_env elsewhere in the plugin's config)",
            settings.api_key_env);
        return;
    }

    registry.Register(std::make_shared<minimal::typesafe_advisor::TypesafeAdvisor>(
        settings, *api_key, minimal::net::HttpClient{}));
}

}  // extern "C"
