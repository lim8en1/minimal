// The collision twin of fixture-service: a different plugin claiming the SAME
// service name. Loaded together they exercise the fatal NameKind::Service
// collision -- fatal, not shadowed, and diagnosable down to both plugins.
#include "minimal/plugins/plugin_abi.h"

namespace {

class FixtureStateServiceB {
public:
    virtual ~FixtureStateServiceB() = default;
};

FixtureStateServiceB& Store() {
    static FixtureStateServiceB store;
    return store;
}

}  // namespace

extern "C" {
const char* minimal_plugin_abi_tag() { return minimal::common::AbiTag(); }
const char* minimal_plugin_name() { return "fixture-service-b"; }
const char* minimal_plugin_version() { return "1.0.0"; }

void minimal_plugin_register_services(
    minimal::plugins::ServiceRegistry& registry,
    const minimal::plugins::PluginRegistrationContext&) {
    registry.Provide("fixture.service", &Store());
}
}
