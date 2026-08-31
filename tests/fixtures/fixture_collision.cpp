// A second plugin registering the SAME tool name as fixture-good ("fixture_tool").
// The collision/alias tests need two plugins that actually collide: loading this
// alongside fixture-good reproduces the exact shape of two plugins both claiming
// `bash`, with the execute() result distinguishing which implementation won.
#include "minimal/plugins/plugin_abi.h"

namespace {

class FixtureTool : public minimal::tools::Tool {
public:
    explicit FixtureTool(std::string echo) : echo_(std::move(echo)) {
        spec_.name = "fixture_tool";
        spec_.category = minimal::tools::ToolCategory::ReadOnly;
        spec_.definition.name = spec_.name;
        spec_.definition.description = "a tool contributed by the colliding fixture plugin";
        spec_.definition.input_schema = nlohmann::json::object();
    }
    const minimal::tools::ToolSpec& spec() const override { return spec_; }
    minimal::tools::ToolResult execute(const nlohmann::json&,
                                       minimal::tools::ToolContext&) override {
        return {false, echo_};
    }
private:
    minimal::tools::ToolSpec spec_;
    std::string echo_;
};

}  // namespace

extern "C" {
const char* minimal_plugin_abi_tag() { return minimal::common::AbiTag(); }
const char* minimal_plugin_name() { return "fixture-collision"; }
const char* minimal_plugin_version() { return "1.0.0"; }

void minimal_plugin_register_tools(
    minimal::tools::ToolRegistry& registry,
    const minimal::plugins::PluginRegistrationContext& context) {
    // Same config-echo contract as fixture-good; the default distinguishes
    // which implementation is alive in the collision/alias tests.
    std::string echo = "collision ok";
    if (context.config.is_object()) {
        if (auto it = context.config.find("echo_value");
            it != context.config.end() && it->is_string()) {
            echo = it->get<std::string>();
        }
    }
    registry.Register(std::make_unique<FixtureTool>(echo));
}
}
