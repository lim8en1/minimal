// The first fixture to exercise a registration seam beyond tools/providers:
// an advisor that allows one tool outright and abstains on everything else, so
// the E2E can prove both directions over a real socket -- the auto-answered
// path and the fall-through to a real wire prompt.
#include "minimal/plugins/plugin_abi.h"

namespace {

class FixtureAdvisor : public minimal::tools::PermissionAdvisor {
public:
    std::optional<minimal::tools::PermissionResponse> Advise(
        const minimal::tools::PermissionRequest& request,
        const minimal::tools::AdvisorContext&) override {
        if (request.tool_name == "auto_tool") {
            return minimal::tools::PermissionResponse{
                minimal::tools::PermissionDecision::Allow, {}};
        }
        return std::nullopt;  // abstain: the human decides
    }
};

}  // namespace

extern "C" {
const char* minimal_plugin_abi_tag() { return minimal::common::AbiTag(); }
const char* minimal_plugin_name() { return "fixture-advisor"; }
const char* minimal_plugin_version() { return "1.0.0"; }

void minimal_plugin_register_permission_advisor(
    minimal::tools::PermissionAdvisorRegistry& registry,
    const minimal::plugins::PluginRegistrationContext&) {
    registry.Register(std::make_shared<FixtureAdvisor>());
}
}
