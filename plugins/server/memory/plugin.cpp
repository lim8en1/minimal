// memory -- durable, cross-session memory the model grows itself: a SQLite
// store with hybrid (bm25 + embedding) search, a controlled tag vocabulary
// filed by a cheap tagger model, and a "Memory" index pre-filled into every
// session's context through the core context-contributor seam.
//
// SHIPPED DISABLED (DefaultsDisabled in plugin_discovery.cpp): it writes to
// disk on the model's initiative and, under the http embedding strategy,
// sends entry text to an external endpoint. Enabling it is one plugins.json
// edit. With no embedding backend and no tagger configured the store still
// works -- search is lexical and entries stay untagged.

#include <filesystem>
#include <memory>
#include <mutex>

#include "memory_runtime.hpp"

#include "minimal/agent/session_support_service.hpp"
#include "minimal/common/abi_tag.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/plugins/plugin_abi.h"

namespace {

// One runtime per process, built by whichever registration hook runs first.
// Registration is single-threaded; the mutex only makes that assumption
// cheap to keep.
std::mutex g_runtime_mu;
std::shared_ptr<minimal::memory::MemoryRuntime> g_runtime;

std::shared_ptr<minimal::memory::MemoryRuntime> Runtime(
    const minimal::plugins::PluginRegistrationContext& context) {
    using namespace minimal::memory;
    std::lock_guard<std::mutex> lock(g_runtime_mu);
    if (g_runtime != nullptr) return g_runtime;

    auto runtime = std::make_shared<MemoryRuntime>();
    runtime->settings = ParseMemorySettings(context.config);
    runtime->store = std::make_unique<MemoryStore>(context.data_dir / "memory.sqlite3");

    auto plan = MakeEmbeddingPlan(runtime->settings, context.data_dir);
    // The one re-embed trigger: rows embedded by a different backend or
    // model go back to pending. With no backend configured, vectors are left
    // alone -- re-enabling the same backend later finds them intact.
    if (plan.factory) runtime->store->ResetStaleSignatures(plan.signature);
    runtime->embeddings = std::make_unique<EmbeddingService>(
        runtime->store.get(), std::move(plan), minimal::common::DefaultLogger());

    // Resolved lazily on the tagger's worker, by which time core services
    // are wired (the worker starts on first use, never at registration).
    minimal::plugins::ServiceRegistry* services = context.services;
    const std::string model_ref = runtime->settings.tagger_model;
    Tagger::Resolver resolver = [services, model_ref]() -> std::optional<TaggerLlm> {
        if (services == nullptr) return std::nullopt;
        void* raw = services->Find(minimal::agent::kSessionSupportServiceName);
        if (raw == nullptr) return std::nullopt;
        auto* support = static_cast<minimal::agent::SessionSupportService*>(raw);
        auto resolved = support->ResolveModelRef(model_ref);
        if (!resolved) return std::nullopt;
        return TaggerLlm{resolved->provider, resolved->model};
    };
    runtime->tagger = std::make_unique<Tagger>(runtime->settings, runtime->store.get(),
                                               runtime->embeddings.get(), std::move(resolver),
                                               minimal::common::DefaultLogger());
    g_runtime = runtime;
    return runtime;
}

}  // namespace

extern "C" {

MINIMAL_PLUGIN_ABI_SYMBOLS("memory")

void minimal_plugin_register_tools(minimal::tools::ToolRegistry& registry,
                                   const minimal::plugins::PluginRegistrationContext& context) {
    auto runtime = Runtime(context);
    registry.Register(minimal::memory::MakeMemorySearchTool(runtime));
    registry.Register(minimal::memory::MakeMemoryGetTool(runtime));
    registry.Register(minimal::memory::MakeMemoryStoreTool(runtime));
    registry.Register(minimal::memory::MakeMemoryUpdateTool(runtime));
    registry.Register(minimal::memory::MakeMemoryDeleteTool(runtime));
}

void minimal_plugin_register_commands(minimal::commands::CommandRegistry& registry,
                                      const minimal::plugins::PluginRegistrationContext& context) {
    registry.Register(minimal::memory::MakeMemoryCommand(Runtime(context)));
}

void minimal_plugin_register_context_contributors(
    minimal::agent::ContextContributorRegistry& registry,
    const minimal::plugins::PluginRegistrationContext& context) {
    registry.Register(minimal::memory::MakeMemoryIndexContributor(Runtime(context)));
}

}  // extern "C"
