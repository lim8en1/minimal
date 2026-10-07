// decision-advisor -- auto-mode as a track-J permission advisor (docs/
// EXTENSIBILITY.md §3): Jev answers the Ask-gated tool calls the substrate
// resolved to "ask a human"; abstentions (review choice, below-threshold
// probabilities, HTTP trouble, cancel) fall through to the wire checker.
//
// Ships DISABLED (DefaultsDisabled in plugin_discovery.cpp): it sends tool
// input to an external endpoint and auto-answers permission prompts, so
// enabling it is the operator's informed decision -- one plugins.json edit
// plus one permissions.json "advisor" field.
//
// The substrate is untouched and untrusting: the advisor can only NARROW what
// reaches a person. AllowOnly short-circuits before the advisor exists (no
// approvals smuggled into headless runs), and ToolExecutor re-filters every
// granted key against the disclosure, so a hostile or hallucinating judge
// mints nothing.

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <utility>

#include "minimal/config/secrets.hpp"
#include "minimal/common/config_paths.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/net/http_client.hpp"
#include "minimal/common/plugin_config_label.hpp"
#include "minimal/common/plugin_config_schema.hpp"
#include "minimal/plugins/plugin_abi.h"
#include "systemone_client.hpp"
#include "minimal/plugins/service_registry.hpp"
#include "minimal/tools/permission_advisor.hpp"

#include "advisor.hpp"
#include "settings.hpp"

MINIMAL_PLUGIN_CONFIG_SCHEMA(
    minimal::common::PluginConfigLabel(minimal::common::kPluginConfigDirName, "decision-advisor"),
    [](const nlohmann::json& config) {
        (void)minimal::decision_advisor::ParseSettings(
            minimal::decision_advisor::LoadBundledSettings(
                minimal::common::ConfigDir() / minimal::decision_advisor::kBundledSettingsFile),
            config);
    })

extern "C" {

MINIMAL_PLUGIN_ABI_SYMBOLS("decision-advisor")

void minimal_plugin_register_permission_advisor(
    minimal::tools::PermissionAdvisorRegistry& registry,
    const minimal::plugins::PluginRegistrationContext& context) {
    // Every value comes from the seeded bundled file under this entry's
    // overrides; a missing file or field throws, which keeps the plugin from
    // loading with the reason named -- there is no built-in fallback.
    const auto settings = minimal::decision_advisor::ParseSettings(
        minimal::decision_advisor::LoadBundledSettings(
            minimal::common::ConfigDir() / minimal::decision_advisor::kBundledSettingsFile),
        context.config);

    // Bound ONCE, here: a reference that can never resolve (an unset env
    // var) is a loud, contained registration failure (the plugin loads
    // degraded, the sanity check warns that the configured advisor resolves
    // to nothing), not a silent 401 on the first gated tool call hours later.
    // core.secrets is core-provided before any plugin loads.
    static const minimal::config::SecretResolver kDefaultSecrets;
    const auto* found =
        context.services != nullptr
            ? static_cast<const minimal::config::SecretResolver*>(
                  context.services->Find(minimal::config::kSecretsServiceName))
            : nullptr;
    minimal::common::SecretHandle api_key;
    try {
        api_key = (found != nullptr ? *found : kDefaultSecrets)
                      .Bind(settings.api_key, settings.endpoint);
    } catch (const std::exception& error) {
        minimal::common::DefaultLogger().error(
            "decision-advisor: api_key {}; the advisor will not register", error.what());
        return;
    }

    std::filesystem::path decision_log;
    if (settings.debug_decision_logging) {
        if (context.data_dir.empty()) {
            minimal::common::DefaultLogger().warn(
                "decision-advisor: debug_decision_logging is on but the host provides no data "
                "dir; decisions will not be logged");
        } else {
            decision_log = context.data_dir / "decisions.jsonl";
            minimal::common::DefaultLogger().info("decision-advisor: logging decisions to {}",
                                                  decision_log.string());
        }
    }

    registry.Register(std::make_shared<minimal::decision_advisor::DecisionAdvisor>(
        settings, std::move(api_key), minimal::net::HttpClient{}, std::move(decision_log)));
}

// One real call, as a tool call would make it, with no retries: the
// endpoint answers, the key is accepted, the answer parses, and the model
// that answered is the one configured. A refusal is put on the field it is
// about; where the backend lists its models (an OpenAI-shaped server beside
// /systemone), a model it does not serve is named.
void minimal_plugin_check_config(const nlohmann::json& config,
                                 const minimal::plugins::ConfigCheckContext& context,
                                 nlohmann::json& out) {
    using minimal::decision_advisor::ParseSettings;
    minimal::plugins::CheckReport report;
    auto settings = ParseSettings(minimal::decision_advisor::LoadBundledSettings(
                                      minimal::common::ConfigDir() /
                                      minimal::decision_advisor::kBundledSettingsFile),
                                  config);
    std::string key;
    if (const auto problem =
            minimal::plugins::ResolveKey(context.services, settings.api_key, settings.endpoint, key)) {
        report.Fail("api_key", *problem);
        out = report.rows();
        return;
    }
    // One attempt, the check's budget (a cold local model loads first).
    settings.max_retries = 0;
    settings.request_timeout_ms = static_cast<int>(context.budget.count());
    settings.budget_ms = settings.request_timeout_ms;
    const nlohmann::json state = {{"tool_name", "read_file"},
                                  {"summary", "read README.md"},
                                  {"tool_input", {{"path", "README.md"}}}};
    const auto outcome = minimal::decision_advisor::AskSystemOne(
        minimal::net::HttpClient{}, settings, key, state, context.cancelled);
    if (outcome.ok) {
        report.Ok("endpoint", "answers (" + std::to_string(outcome.total.count()) + " ms)");
        if (!settings.api_key.empty()) report.Ok("api_key", "accepted");
        if (!outcome.answered_model.empty() && outcome.answered_model != settings.model) {
            report.Warn("model", "the backend answered as " + outcome.answered_model);
        } else {
            report.Ok("model", settings.model);
        }
        out = report.rows();
        return;
    }
    const std::string& reason = outcome.abstain_reason;
    // The reason carries the status first ("HTTP 401 (invalid key)").
    if (reason.rfind("HTTP 401", 0) == 0 || reason.rfind("HTTP 403", 0) == 0) {
        report.Fail("api_key", "the backend refused the key (" + reason + ")");
    } else if (reason == "budget exhausted") {
        report.Fail("endpoint", "no answer within " + std::to_string(std::chrono::duration_cast<std::chrono::seconds>(context.budget).count()) + " s");
    } else if (reason == "unparseable response body" ||
               reason == "response carries no 'action' choice answer") {
        report.Fail("endpoint", "answered, but not as a systemone API (" + reason + ") -- the wrong URL?");
    } else {
        report.Fail("endpoint", reason);
    }
    // A model the server does not serve is the usual cause of a 4xx from a
    // local router: ask the server what it serves, where it says.
    if (reason.rfind("HTTP 4", 0) == 0) {
        const auto slash = settings.endpoint.rfind('/');
        if (slash != std::string::npos) {
            std::string problem;
            if (const auto served = minimal::plugins::ListOpenAiModels(
                    settings.endpoint.substr(0, slash), key, context.budget, context.cancelled, problem)) {
                if (const auto missing = minimal::plugins::ModelNotServed(settings.model, *served)) {
                    report.Fail("model", *missing);
                }
            }
        }
    }
    out = report.rows();
}

}  // extern "C"
