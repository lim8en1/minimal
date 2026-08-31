// Provides one service under the track-L naming convention, through the
// shipped-abstract-interface shape a real provider uses (header-only interface
// implemented inside this .so; the consumer casts Find's result to the header
// type and calls through the vtable).
#include "minimal/plugins/plugin_abi.h"

#include <string>
#include <vector>

namespace {

// The contract this fixture ships to consumers. Header-only, pure virtual --
// the same shape as tools-bash's bash_state_service.hpp.
class FixtureStateService {
public:
    virtual ~FixtureStateService() = default;
    virtual void Erase(const std::string& session_id) = 0;
    virtual bool Has(const std::string& session_id) const = 0;
};

class FixtureStateStore : public FixtureStateService {
public:
    void Erase(const std::string& session_id) override { erased_.push_back(session_id); }
    bool Has(const std::string& session_id) const override {
        for (const auto& id : erased_) {
            if (id == session_id) return true;
        }
        return false;
    }
private:
    std::vector<std::string> erased_;
};

FixtureStateStore& Store() {
    static FixtureStateStore store;
    return store;
}

}  // namespace

extern "C" {
const char* minimal_plugin_abi_tag() { return minimal::common::AbiTag(); }
const char* minimal_plugin_name() { return "fixture-service"; }
const char* minimal_plugin_version() { return "1.0.0"; }

void minimal_plugin_register_services(
    minimal::plugins::ServiceRegistry& registry,
    const minimal::plugins::PluginRegistrationContext& context) {
    registry.Provide("fixture.service", &Store());
    // The CONSUMER side of the channel: PluginRegistrationContext::services must
    // be the very registry the loader dispatches register_services into, so a
    // plugin can capture the pointer at registration and Find lazily later.
    // Provided only when the identity holds, so the loader test can observe it.
    if (context.services == &registry) {
        registry.Provide("fixture.context_services_ok", &Store());
    }
}
}
