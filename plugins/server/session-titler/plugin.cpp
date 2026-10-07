// session-titler -- the ARCHITECTURE.md "Session naming, layered" second
// layer, as an observer plugin exactly as designed there. Ships disabled
// (DefaultsDisabled in plugin_discovery.cpp): it is the one bundled plugin
// that spends the user's tokens unprompted, so enabling it is a decision.

#include "minimal/agent/session_observer_registry.hpp"
#include "minimal/common/abi_tag.hpp"
#include "minimal/common/live_value.hpp"
#include "minimal/common/plugin_config_label.hpp"
#include "minimal/common/plugin_config_schema.hpp"
#include "minimal/plugins/plugin_abi.h"

#include "titler.hpp"

namespace {

using minimal::session_titler::TitlerConfig;

// What the observer reads per title; swapped by minimal_plugin_reconfigure.
std::shared_ptr<minimal::common::LiveValue<TitlerConfig>>& LiveConfig() {
    static std::shared_ptr<minimal::common::LiveValue<TitlerConfig>> config;
    return config;
}

}  // namespace

MINIMAL_PLUGIN_CONFIG_SCHEMA(
    minimal::common::PluginConfigLabel(minimal::common::kPluginConfigDirName, "session-titler"),
    [](const nlohmann::json& config) {
        (void)minimal::session_titler::ParseTitlerConfig(config);
    })

extern "C" {

MINIMAL_PLUGIN_ABI_SYMBOLS("session-titler")

void minimal_plugin_register_session_observer(
    minimal::agent::SessionObserverRegistry& registry,
    const minimal::plugins::PluginRegistrationContext& context) {
    LiveConfig() = std::make_shared<minimal::common::LiveValue<TitlerConfig>>(
        minimal::session_titler::ParseTitlerConfig(context.config));
    // The ServiceRegistry pointer is captured at registration and resolved
    // lazily (Find on the titling worker) per the service contract -- this plugin
    // may load before or after any other, and the registry is sealed long
    // before the first session exists.
    registry.Register(std::make_shared<minimal::session_titler::TitlerObserver>(
        LiveConfig(), context.services, minimal::common::DefaultLogger()));
}

// Every field is live: the next title request reads the new values.
bool minimal_plugin_reconfigure(const nlohmann::json& config, std::string& error) {
    if (!LiveConfig()) {
        error = "session-titler is not registered";
        return false;
    }
    try {
        LiveConfig()->Set(minimal::session_titler::ParseTitlerConfig(config));
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

// The model override resolves through providers.json (only the running
// server can say: a disabled titler is told so).
void minimal_plugin_check_config(const nlohmann::json& config,
                                 const minimal::plugins::ConfigCheckContext& context,
                                 nlohmann::json& out) {
    minimal::plugins::CheckReport report;
    const auto settings = minimal::session_titler::ParseTitlerConfig(config);
    if (settings.model.empty()) {
        report.Ok("model", "titles use each session's own model");
    } else if (const auto problem = minimal::plugins::ModelRefProblem(context.services, settings.model)) {
        if (problem->rfind("not checked", 0) == 0) {
            report.Warn("model", *problem);
        } else {
            report.Fail("model", *problem);
        }
    } else {
        report.Ok("model", settings.model + " resolves");
    }
    out = report.rows();
}

}  // extern "C"
