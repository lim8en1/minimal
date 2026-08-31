// A plugin whose registration callback THROWS.
//
// Not a mock, because the failure being tested is what happens to the process:
// the callback used to be invoked bare, so the exception propagated out of
// LoadOne, out of LoadAll, into main() -- which has no handler -- and killed the
// server with no diagnostic naming the plugin. A fake loader cannot demonstrate
// that; only dlopen'ing something that really throws can.
//
// It registers one tool FIRST and then throws, so the test can also see that the
// attribution window was closed on the way out: a window left open would
// mis-attribute every later plugin's registrations to this one.
#include <stdexcept>

#include "minimal/plugins/plugin_abi.h"

namespace {

class BeforeTheThrow : public minimal::tools::Tool {
public:
    BeforeTheThrow() {
        spec_.name = "registered_before_throw";
        spec_.category = minimal::tools::ToolCategory::ReadOnly;
        spec_.definition.name = spec_.name;
        spec_.definition.description = "registered just before the callback throws";
        spec_.definition.input_schema = nlohmann::json::object();
    }
    const minimal::tools::ToolSpec& spec() const override { return spec_; }
    minimal::tools::ToolResult execute(const nlohmann::json&,
                                       minimal::tools::ToolContext&) override {
        return {false, "ok"};
    }
private:
    minimal::tools::ToolSpec spec_;
};

}  // namespace

extern "C" {
const char* minimal_plugin_abi_tag() { return minimal::common::AbiTag(); }
const char* minimal_plugin_name() { return "fixture-throws"; }
const char* minimal_plugin_version() { return "1.0.0"; }

void minimal_plugin_register_tools(minimal::tools::ToolRegistry& registry,
                                   const minimal::plugins::ToolRegistrationContext&) {
    registry.Register(std::make_unique<BeforeTheThrow>());
    throw std::runtime_error("this plugin's registration is broken");
}
}
