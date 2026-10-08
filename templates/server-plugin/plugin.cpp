// @PLUGIN_NAME@ -- a user server plugin (scaffold).
//
// The three required symbols below are the capability floor: a plugin missing
// any of them is skipped, not loaded. Every minimal_plugin_register_* hook is
// OPTIONAL -- the loader dlsym's each name and calls it only if found; symbol
// presence IS the declaration, there is no manifest. abi_tag returns
// minimal::common::AbiTag() (never a literal): the tag must report the
// toolchain this binary was actually built with, or the "you forgot to
// rebuild" guard means nothing.
//
// Other registration hooks you can add as needed (see
// src/server/plugins/include/minimal/plugins/plugin_abi.h):
//   minimal_plugin_register_provider_factories  -- new provider "backend" kinds
//   minimal_plugin_register_session_observer    -- session lifecycle events
//   minimal_plugin_register_commands            -- "/"-commands
//   minimal_plugin_register_services            -- cross-plugin pull services
//   minimal_plugin_register_mention_resolver    -- "@"-mentions (exclusive)
//   minimal_plugin_register_auth_provider       -- (exclusive)
//   minimal_plugin_register_permission_advisor  -- auto-answers Ask decisions
//   minimal_plugin_register_model_catalogs      -- lazy model discovery
//   minimal_plugin_register_compaction_strategies
//   minimal_plugin_register_hook_points         -- new veto-capable hook events
//
// Config reaches you as context.config: the verbatim "config" object on this
// plugin's entry in plugins.json.

#include <memory>
#include <string>
#include <utility>

#include "minimal/plugins/plugin_abi.h"

namespace {

// One trivial tool, end to end: spec (name/category/schema the model sees) +
// execute. Category feeds permission decisions; ReadOnly needs no approval.
class HelloTool final : public minimal::tools::Tool {
public:
    HelloTool() {
        spec_.name = "@PLUGIN_NAME@_hello";
        spec_.category = minimal::tools::ToolCategory::ReadOnly;
        spec_.definition.name = spec_.name;
        spec_.definition.description = "says hello (scaffold example; replace me)";
        spec_.definition.input_schema = {
            {"type", "object"},
            {"properties",
             {{"name", {{"type", "string"}, {"description", "who to greet"}}}}},
        };
    }

    const minimal::tools::ToolSpec& spec() const override { return spec_; }

    minimal::tools::ToolResult execute(const nlohmann::json& args,
                                       minimal::tools::ToolContext&) override {
        const auto who = args.value("name", std::string{"world"});
        return {/*is_error=*/false, "hello, " + who};
    }

private:
    minimal::tools::ToolSpec spec_;
};

}  // namespace

extern "C" {

// The tag and version baked into THIS .so at compile time -- not
// common::AbiTag(), which binds to the running host's copy at dlopen and
// would always match it, letting a stale plugin load.
MINIMAL_PLUGIN_ABI_SYMBOLS("@PLUGIN_NAME@")

void minimal_plugin_register_tools(
    minimal::tools::ToolRegistry& registry,
    const minimal::plugins::PluginRegistrationContext& context) {
    (void)context;  // context.config is this plugin's own config object
    registry.Register(std::make_unique<HelloTool>());
}

}  // extern "C"
