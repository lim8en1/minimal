// A plugin with a config schema, one live field and one restart field, and a
// tool that reports the values it is running with -- so the config endpoint's
// tests can watch a live change land, and a restart change not land.

#include "minimal/common/config_reader.hpp"
#include "minimal/common/live_value.hpp"
#include "minimal/common/plugin_config_label.hpp"
#include "minimal/common/plugin_config_schema.hpp"
#include "minimal/plugins/plugin_abi.h"

namespace {

struct Settings {
    std::int64_t limit = 5;   // live
    std::string mode = "fast";  // restart
};

Settings Parse(const nlohmann::json& config) {
    Settings settings;
    minimal::config::Reader reader(
        config, minimal::common::PluginConfigLabel(minimal::common::kPluginConfigDirName,
                                                   "fixture-config"));
    reader.Describe("limit", {"A live number.", minimal::config::ApplyPolicy::kLive});
    reader.Describe("mode", {"A restart choice."});
    settings.limit = reader.Int("limit", settings.limit, 1, 100);
    settings.mode = reader.Enum("mode", settings.mode, {"fast", "slow"});
    reader.Finish();
    return settings;
}

std::shared_ptr<minimal::common::LiveValue<Settings>>& Live() {
    static std::shared_ptr<minimal::common::LiveValue<Settings>> live;
    return live;
}

class ValuesTool : public minimal::tools::Tool {
public:
    ValuesTool() {
        spec_.name = "fixture_config_values";
        spec_.definition.name = spec_.name;
        spec_.definition.description = "Reports the plugin's running config.";
        spec_.definition.input_schema = {{"type", "object"}};
    }
    const minimal::tools::ToolSpec& spec() const override { return spec_; }
    minimal::tools::ToolResult execute(const nlohmann::json&, minimal::tools::ToolContext&) override {
        const auto settings = Live()->Get();
        return {false, std::to_string(settings->limit) + " " + settings->mode};
    }

private:
    minimal::tools::ToolSpec spec_;
};

}  // namespace

MINIMAL_PLUGIN_CONFIG_SCHEMA(
    minimal::common::PluginConfigLabel(minimal::common::kPluginConfigDirName, "fixture-config"),
    [](const nlohmann::json& config) { (void)Parse(config); })

extern "C" {

MINIMAL_PLUGIN_ABI_SYMBOLS("fixture-config")

void minimal_plugin_register_tools(minimal::tools::ToolRegistry& registry,
                                   const minimal::plugins::PluginRegistrationContext& context) {
    Live() = std::make_shared<minimal::common::LiveValue<Settings>>(Parse(context.config));
    registry.Register(std::make_unique<ValuesTool>());
}

bool minimal_plugin_reconfigure(const nlohmann::json& config, std::string& error) {
    try {
        Live()->Set(Parse(config));
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

}  // extern "C"
