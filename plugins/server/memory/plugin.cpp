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

}  // extern "C"
