// The decision-advisor E2E fixture: the REAL DecisionAdvisor behind a fixture
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
const char* minimal_plugin_name() { return "decision-advisor-fixture"; }
const char* minimal_plugin_version() { return "1.0.0"; }

void minimal_plugin_register_permission_advisor(
    minimal::tools::PermissionAdvisorRegistry& registry,
    const minimal::plugins::PluginRegistrationContext& context) {
    // The SHIPPED bundled file (the source tree's, not a config dir's) under
    // the E2E's config slice, which carries its loopback stub URL.
    auto settings = minimal::decision_advisor::ParseSettings(
        minimal::decision_advisor::LoadBundledSettings(DECISION_ADVISOR_BUNDLED_FILE),
        context.config);
    settings.max_retries = 0;        // deterministic: exactly one attempt
    settings.backoff_initial_ms = 1;
    settings.request_timeout_ms = 1000;
    settings.budget_ms = 1000;

    registry.Register(std::make_shared<minimal::decision_advisor::DecisionAdvisor>(
        settings, minimal::common::SecretHandle::Literal("fixture-key"),
        minimal::net::HttpClient{}));
}

}  // extern "C"
