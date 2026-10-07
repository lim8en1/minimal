// memory -- durable, cross-session memory the model grows itself: a SQLite
// store with hybrid (bm25 + embedding) search, a controlled tag vocabulary
// filed by a cheap tagger model, and a "Memory" index pre-filled into every
// session's context through the core context-contributor seam.
//
// With no embedding backend and no tagger configured the store still works --
// search is lexical and entries stay untagged. If the store cannot open at
// all, the plugin degrades to a /memory command that says why: a bundled
// plugin whose registration throws aborts server startup.

#include <memory>
#include <mutex>
#include <string>

#include "memory_runtime.hpp"

#include "minimal/common/abi_tag.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/common/plugin_config_label.hpp"
#include "minimal/common/plugin_config_schema.hpp"
#include "minimal/plugins/plugin_abi.h"

namespace {

// One runtime per process, built by whichever registration hook runs first.
// Registration is single-threaded; the mutex only makes that assumption
// cheap to keep. A failed build is remembered, so it is attempted and
// reported once.
std::mutex g_runtime_mu;
bool g_built = false;
std::shared_ptr<minimal::memory::MemoryRuntime> g_runtime;
std::string g_error;

std::shared_ptr<minimal::memory::MemoryRuntime> Runtime(
    const minimal::plugins::PluginRegistrationContext& context) {
    std::lock_guard<std::mutex> lock(g_runtime_mu);
    if (!g_built) {
        g_built = true;
        g_runtime = minimal::memory::BuildMemoryRuntime(context.config, context.data_dir,
                                                        context.services, g_error);
        if (g_runtime == nullptr) {
            minimal::common::DefaultLogger().warn(
                "memory: disabled for this run, the store could not be opened: {}", g_error);
        }
    }
    return g_runtime;
}

}  // namespace

MINIMAL_PLUGIN_CONFIG_SCHEMA(
    minimal::common::PluginConfigLabel(minimal::common::kPluginConfigDirName, "memory"),
    [](const nlohmann::json& config) {
        (void)minimal::memory::ParseMemorySettings(config);
    })

extern "C" {

MINIMAL_PLUGIN_ABI_SYMBOLS("memory")

// Only "live" fields reach here -- limits read per use by the tools, the
// tagger and the context index -- so swapping the whole parsed struct leaves
// the embedding backend and the tools' shape as they were built.
bool minimal_plugin_reconfigure(const nlohmann::json& config, std::string& error) {
    std::shared_ptr<minimal::memory::MemoryRuntime> runtime;
    {
        std::lock_guard<std::mutex> lock(g_runtime_mu);
        runtime = g_runtime;
    }
    if (runtime == nullptr) {
        error = g_error.empty() ? "memory is not running" : "memory is disabled: " + g_error;
        return false;
    }
    try {
        runtime->live->Set(minimal::memory::ParseMemorySettings(config));
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

void minimal_plugin_register_tools(minimal::tools::ToolRegistry& registry,
                                   const minimal::plugins::PluginRegistrationContext& context) {
    auto runtime = Runtime(context);
    if (runtime == nullptr) return;
    registry.Register(minimal::memory::MakeMemorySearchTool(runtime));
    registry.Register(minimal::memory::MakeMemoryGetTool(runtime));
    registry.Register(minimal::memory::MakeMemoryStoreTool(runtime));
    registry.Register(minimal::memory::MakeMemoryUpdateTool(runtime));
    registry.Register(minimal::memory::MakeMemoryDeleteTool(runtime));
}

void minimal_plugin_register_commands(minimal::commands::CommandRegistry& registry,
                                      const minimal::plugins::PluginRegistrationContext& context) {
    auto runtime = Runtime(context);
    if (runtime == nullptr) {
        registry.Register(minimal::memory::MakeMemoryUnavailableCommand(g_error));
        return;
    }
    registry.Register(minimal::memory::MakeMemoryCommand(runtime));
}

void minimal_plugin_register_context_contributors(
    minimal::agent::ContextContributorRegistry& registry,
    const minimal::plugins::PluginRegistrationContext& context) {
    auto runtime = Runtime(context);
    if (runtime == nullptr) return;
    registry.Register(minimal::memory::MakeMemoryIndexContributor(runtime));
}

// The embedding backend the strategy picks, for real: an http backend
// lists the model (where it lists models at all) and embeds a word; a
// sidecar's interpreter runs; the tagger's model ref resolves.
void minimal_plugin_check_config(const nlohmann::json& config,
                                 const minimal::plugins::ConfigCheckContext& context,
                                 nlohmann::json& out) {
    using minimal::memory::StrategyKind;
    minimal::plugins::CheckReport report;
    const auto settings = minimal::memory::ParseMemorySettings(config);
    StrategyKind strategy = settings.strategy;
    if (strategy == StrategyKind::kAuto) {
        strategy = !settings.sidecar_model.empty() ? StrategyKind::kSidecar
                   : !settings.api_base.empty()    ? StrategyKind::kHttp
                                                   : StrategyKind::kNone;
    }
    if (strategy == StrategyKind::kHttp) {
        std::string base = settings.api_base;
        while (!base.empty() && base.back() == '/') base.pop_back();
        std::string key;
        if (const auto problem = minimal::plugins::ResolveKey(context.services, settings.api_key,
                                                              base + "/embeddings", key)) {
            report.Fail("api_key", *problem);
        } else {
            const auto share = context.budget / 2;
            std::string listing_problem;
            if (const auto served = minimal::plugins::ListOpenAiModels(base, key, share,
                                                                       context.cancelled,
                                                                       listing_problem)) {
                if (const auto missing = minimal::plugins::ModelNotServed(settings.model, *served)) {
                    report.Fail("model", *missing);
                }
            }
            const nlohmann::json request = {{"model", settings.model}, {"input", {"check"}}};
            const auto probe = minimal::plugins::ProbeHttp(base + "/embeddings", key, share,
                                                           context.cancelled, true, request.dump());
            if (probe.ok) {
                report.Ok("api_base", "embeds with " + settings.model);
            } else {
                report.Fail(probe.status == 401 || probe.status == 403 ? "api_key" : "api_base",   // NOLINT(readability-magic-numbers): HTTP status codes
                            probe.problem);
            }
        }
    } else if (strategy == StrategyKind::kSidecar) {
        const std::string python =
            settings.sidecar_python_path.empty() ? "python3" : settings.sidecar_python_path;
        if (const auto problem = minimal::plugins::ExecutableProblem(python)) {
            report.Fail("sidecar_python_path", *problem);
        } else {
            report.Ok("sidecar_python_path", python + " runs (fastembed itself is checked when the sidecar starts)");
        }
    } else {
        report.Ok("strategy", "no embedding backend: lexical search");
    }
    if (!settings.tagger_model.empty()) {
        if (const auto problem = minimal::plugins::ModelRefProblem(context.services, settings.tagger_model)) {
            if (problem->rfind("not checked", 0) == 0) {
                report.Warn("tagger_model", *problem);
            } else {
                report.Fail("tagger_model", *problem);
            }
        } else {
            report.Ok("tagger_model", settings.tagger_model + " resolves");
        }
    }
    out = report.rows();
}

}  // extern "C"
