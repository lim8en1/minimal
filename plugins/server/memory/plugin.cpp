// memory -- the dynamic-skill / dynamic-context plugin: a durable,
// DB-backed lesson store the model grows itself, with embedding-similarity
// search (http or onnx backends behind one seam), a capped per-scope topic
// taxonomy, and an optional small-LLM enrichment pass (summary + topic).
//
// SHIPPED DISABLED (DefaultsDisabled in plugin_discovery.cpp): it writes to
// disk on the model's initiative and, under the http embedding strategy,
// sends entry text to an external endpoint. Enabling it is one plugins.json
// edit. With no embedding backend configured the store still works -- search
// degrades to the lexical prefilter, and the tools report that state.

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>

#include <nlohmann/json.hpp>

#include "embedding.hpp"
#include "embedding_http.hpp"
#include "memory_settings.hpp"
#include "memory_store.hpp"
#include "memory_tool.hpp"
#include "minimal/commands/command.hpp"
#include "minimal/commands/command_registry.hpp"
#include "minimal/common/abi_tag.hpp"
#include "minimal/plugins/plugin_abi.h"

extern "C" {

MINIMAL_PLUGIN_ABI_SYMBOLS("memory")

namespace {

// The tools are stateless over this shared runtime; the extern "C" callbacks
// run at registration and at tool dispatch, so one instance per process is
// the right shape (checklist's static-store pattern). Guarded by mu_ against
// nothing in practice -- registration is single-threaded -- but a mutex costs
// less than an argument.
std::mutex g_runtime_mu;
std::shared_ptr<minimal::memory::MemoryRuntime> g_runtime;
std::filesystem::path g_data_dir;

void BuildRuntime(const minimal::plugins::PluginRegistrationContext& context) {
    std::lock_guard<std::mutex> lock(g_runtime_mu);
    auto runtime = std::make_shared<minimal::memory::MemoryRuntime>();
    runtime->settings = minimal::memory::ParseMemorySettings(context.config);
    runtime->services = context.services;

    runtime->store = std::make_unique<minimal::memory::MemoryStore>(
        context.data_dir / "memory.sqlite3");
    runtime->enricher = std::make_unique<minimal::memory::Enricher>(
        runtime->settings, runtime->store.get());

    // Strategy resolution. "auto" walks onnx -> http -> none, reporting the
    // winner through Describe() (never a silent fallback: /memory and the
    // digest name which backend actually won and the reason the others were
    // skipped is derivable from config).
    const auto& s = runtime->settings;
    auto strategy = s.strategy;
    if (strategy == minimal::memory::StrategyKind::kAuto) {
        if (!s.model_path.empty() && std::filesystem::exists(context.data_dir / s.model_path)) {
            strategy = minimal::memory::StrategyKind::kOnnx;
        } else if (!s.api_base.empty()) {
            strategy = minimal::memory::StrategyKind::kHttp;
        } else {
            strategy = minimal::memory::StrategyKind::kNone;
        }
    }
    switch (strategy) {
        case minimal::memory::StrategyKind::kHttp:
            if (s.api_base.empty()) break;  // unconfigured; degrade to none
            runtime->embedder = std::make_unique<minimal::memory::HttpEmbeddingFunction>(
                s, minimal::net::HttpClient{});
            break;
        case minimal::memory::StrategyKind::kOnnx:
            // Model load failures leave embedder null: the tools answer
            // "lexical only" rather than failing the plugin load.
            runtime->embedder = minimal::memory::TryMakeOnnxEmbeddingFunction(
                s, context.data_dir);
            break;
        default:
            break;  // kNone / unconfigured auto
    }
    runtime->signature = runtime->embedder ? runtime->embedder->Signature() : "none";

    // A strategy change flips existing rows back to pending (the reindex
    // trigger); the count is what /memory reports.
    if (runtime->embedder) runtime->store->SetSignatureForAll(runtime->signature, 0);

    g_data_dir = context.data_dir;
    g_runtime = std::move(runtime);
}

}  // namespace

void minimal_plugin_register_tools(
    minimal::tools::ToolRegistry& registry,
    const minimal::plugins::PluginRegistrationContext& context) {
    BuildRuntime(context);
    auto runtime = std::atomic_load(&g_runtime);
    registry.Register(minimal::memory::MakeMemorySearchTool(runtime));
    registry.Register(minimal::memory::MakeMemoryStoreTool(runtime));
    registry.Register(minimal::memory::MakeMemoryGetTool(runtime));
    registry.Register(minimal::memory::MakeMemoryDeleteTool(runtime));
}

void minimal_plugin_register_commands(
    minimal::commands::CommandRegistry& registry,
    const minimal::plugins::PluginRegistrationContext& context) {
    if (g_runtime == nullptr) BuildRuntime(context);
    registry.Register(minimal::memory::MakeMemoryCommand(std::atomic_load(&g_runtime)));
    registry.SetAttribution("memory");
}

}  // extern "C"
