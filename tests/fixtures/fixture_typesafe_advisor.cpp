// The typesafe-advisor E2E fixture: the REAL TypesafeAdvisor behind a fixture
// .so, so session_service_test's advisor E2E can load it through the real
// plugin loader and exercise the full path -- registration attribution, config
// slice, composition, and an actual Ask resolved over the wire -- against a
// loopback TypeSafe stub the test controls via the plugin's own config
// ("endpoint").
//
// Unlike fixture_advisor.cpp (a hand-rolled advisor proving the seam), the
// advisor HERE is the shipped code: same parse, same thresholds, same retry
// loop, same grant behavior. Only the endpoint points at the test's server.
#include <cstdlib>
#include <memory>
#include <utility>

#include "minimal/common/logger.hpp"
#include "minimal/net/http_client.hpp"
#include "minimal/plugins/plugin_abi.h"
#include "minimal/tools/permission_advisor.hpp"

#include "advisor.hpp"
#include "settings.hpp"

extern "C" {

const char* minimal_plugin_abi_tag() { return minimal::common::AbiTag(); }
const char* minimal_plugin_name() { return "typesafe-advisor-fixture"; }
const char* minimal_plugin_version() { return "1.0.0"; }

void minimal_plugin_register_permission_advisor(
    minimal::tools::PermissionAdvisorRegistry& registry,
    const minimal::plugins::PluginRegistrationContext& context) {
    auto settings = minimal::typesafe_advisor::ParseSettings(context.config);
    // The E2E passes its loopback stub URL; defaults for everything it omits.
    settings.max_retries = 0;        // deterministic: exactly one attempt
    settings.backoff_initial_ms = 1;
    settings.request_timeout_ms = 1000;
    settings.budget_ms = 1000;

    const char* api_key = std::getenv(settings.api_key_env.c_str());
    if (api_key == nullptr || *api_key == '\0') api_key = "fixture-key";

    registry.Register(std::make_shared<minimal::typesafe_advisor::TypesafeAdvisor>(
        settings, api_key, minimal::net::HttpClient{}));
}

}  // extern "C"
