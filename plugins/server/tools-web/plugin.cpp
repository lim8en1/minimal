// tools-web -- web_fetch and web_search.
//
// One plugin, two tools: they share the HTTP client, the settings parsing,
// the URL policy, and (for web_fetch) the truncation path -- splitting them
// would duplicate all of it for a genuinely small feature. web_search is
// inert and self-describing when its backend is unconfigured (its own
// error message names the config key), which is what a separate optional
// plugin would have bought anyway.
//
// Both tools are ReadOnly and declare network_access_required(): the
// `network` dimension (permission.hpp, ARCHITECTURE.md's Permission model)
// is their gate -- refined per host by this plugin's own permission model
// (host_permission_model.hpp), which falls back to core's scalar. An operator who allows Execute for a sandboxed bash does
// not thereby allow fetching arbitrary URLs, and allowing the web tools
// (`network: allow`) does not require allowing bash.
//
// Result size: nothing tool-local is needed beyond HttpClient's own 8 MiB
// buffered-response cap -- the agent loop caps every tool's persisted
// result at one central chokepoint (agent_loop.cpp, "cap tool RESULTS"),
// regardless of which tool produced it.

#include "minimal/common/live_value.hpp"
#include "minimal/common/plugin_config_label.hpp"
#include "minimal/common/plugin_config_schema.hpp"
#include "minimal/plugins/plugin_abi.h"
#include "minimal/plugins/service_registry.hpp"

#include "fetch_tool.hpp"
#include "host_permission_model.hpp"
#include "search_tool.hpp"
#include "web_settings.hpp"

namespace {

using minimal::tools_web::WebSettings;

// The settings both tools read per call, and the secret resolver the parse
// binds search.api_key through -- kept for reconfigure, which gets no context.
struct Live {
    std::shared_ptr<minimal::common::LiveValue<WebSettings>> settings;
    const minimal::config::SecretResolver* secrets = nullptr;
};

Live& State() {
    static Live state;
    return state;
}

}  // namespace

MINIMAL_PLUGIN_CONFIG_SCHEMA(
    minimal::common::PluginConfigLabel(minimal::common::kPluginConfigDirName, "tools-web"),
    [](const nlohmann::json& config) { (void)minimal::tools_web::ParseWebSettings(config); })

extern "C" {

MINIMAL_PLUGIN_ABI_SYMBOLS("tools-web")

void minimal_plugin_register_tools(
    minimal::tools::ToolRegistry& registry,
    const minimal::plugins::PluginRegistrationContext& context) {
    // core.secrets is core-provided before any plugin loads.
    const auto* secrets =
        context.services != nullptr
            ? static_cast<const minimal::config::SecretResolver*>(
                  context.services->Find(minimal::config::kSecretsServiceName))
            : nullptr;
    State().secrets = secrets;
    State().settings = std::make_shared<minimal::common::LiveValue<WebSettings>>(
        minimal::tools_web::ParseWebSettings(context.config, secrets));

    registry.Register(
        minimal::tools_web::MakeWebFetchTool(State().settings, minimal::net::HttpClient{}));
    registry.Register(
        minimal::tools_web::MakeWebSearchTool(State().settings, minimal::net::HttpClient{}));
}

// Only "live" fields reach here (fetch.timeout_ms, fetch.max_redirects,
// search.timeout_ms), so swapping the whole parsed struct changes nothing
// else: the descriptions the tools were built with stay true.
bool minimal_plugin_reconfigure(const nlohmann::json& config, std::string& error) {
    if (!State().settings) {
        error = "tools-web is not registered";
        return false;
    }
    try {
        State().settings->Set(minimal::tools_web::ParseWebSettings(config, State().secrets));
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

// Per-host rules for this plugin's own tools (host_permission_model.hpp):
// the network dimension, finer than core's one scalar.
void minimal_plugin_register_permission_models(
    minimal::tools::PermissionModelRegistry& registry,
    const minimal::plugins::PluginRegistrationContext& context) {
    registry.Register(minimal::tools_web::MakeHostPermissionModel(
        minimal::tools_web::ParseWebSettings(context.config)));
}

// The search API answers a query, with its key, as JSON.
void minimal_plugin_check_config(const nlohmann::json& config,
                                 const minimal::plugins::ConfigCheckContext& context,
                                 nlohmann::json& out) {
    minimal::plugins::CheckReport report;
    const auto* resolver =
        context.services != nullptr
            ? static_cast<const minimal::config::SecretResolver*>(
                  context.services->Find(minimal::config::kSecretsServiceName))
            : nullptr;
    const auto settings = minimal::tools_web::ParseWebSettings(config, resolver);
    if (settings.search_api_url.empty()) {
        report.Ok("search.api_url", "web search is off (no search API)");
        out = report.rows();
        return;
    }
    std::string bearer;
    if (settings.search_api_key.configured()) {
        try {
            bearer = settings.search_api_key.Get().value;
        } catch (const std::exception& error) {
            report.Fail("search.api_key", error.what());
            out = report.rows();
            return;
        }
    }
    // A query may be billed (a paid search API): only when asked for (`c`).
    if (!context.on_demand) {
        report.Ok("search.api_url", "not queried on save (a search may cost) -- press c to check "
                                    "it for real");
        out = report.rows();
        return;
    }
    const std::string separator = settings.search_api_url.find('?') == std::string::npos ? "?" : "&";
    const auto probe = minimal::plugins::ProbeHttp(
        settings.search_api_url + separator + "q=minimal&count=1", bearer, context.remaining(),
        context.cancelled);
    if (probe.ok) {
        report.Ok("search.api_url", "answers a query");
    } else {
        report.Fail(probe.status == 401 || probe.status == 403 ? "search.api_key" : "search.api_url",   // NOLINT(readability-magic-numbers): HTTP status codes
                    probe.problem);
    }
    out = report.rows();
}

}  // extern "C"
