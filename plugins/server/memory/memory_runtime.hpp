#pragma once

// The plugin's shared state and its entry points: one store, one embedding
// service (possibly unconfigured = lexical only), one tagger (possibly
// disabled), shared by the tools, the /memory command and the context index.

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

#include "embedding_service.hpp"
#include "memory_settings.hpp"
#include "memory_store.hpp"
#include "tagger.hpp"

#include "minimal/agent/context_contributor.hpp"
#include "minimal/commands/command.hpp"

namespace minimal::tools {
class Tool;
}
namespace minimal::plugins {
class ServiceRegistry;
}

namespace minimal::memory {

struct MemoryRuntime {
    // Declaration order is destruction order reversed: the tagger (which
    // uses both) stops first, then the embedding worker, then the store.
    MemorySettings settings;
    std::unique_ptr<MemoryStore> store;
    std::unique_ptr<EmbeddingService> embeddings;  // non-null
    std::unique_ptr<Tagger> tagger;                // non-null; inert when disabled

    // Workers start on first use, not at registration: the tagger resolves
    // its model through a core service that is only wired after plugins
    // load, and a server that never touches memory should not spawn the
    // embedding sidecar.
    std::once_flag workers_started;
};

// Builds the whole runtime from the plugin's config. Never throws: on any
// failure (the store cannot open -- corrupt, unreadable, written by a newer
// binary; disk errors) it returns null and fills `error`. A bundled plugin
// whose registration throws aborts server startup, and an optional feature
// must never take the harness down with it -- the caller degrades instead.
std::shared_ptr<MemoryRuntime> BuildMemoryRuntime(const nlohmann::json& config,
                                                  const std::filesystem::path& data_dir,
                                                  plugins::ServiceRegistry* services,
                                                  std::string& error);

// Idempotent; every entry point (tools, command, context index) calls it.
inline void StartWorkers(MemoryRuntime& runtime) {
    std::call_once(runtime.workers_started, [&] {
        runtime.embeddings->Start();
        runtime.tagger->Start();
    });
}

// scope=project resolves to the enclosing worktree root: walk up from the
// session root to the nearest directory holding .git (a directory normally,
// a FILE in a linked worktree), falling back to the session root itself. So
// project scope is stable across subdirectories, and works without git.
std::filesystem::path WorktreeRoot(const std::filesystem::path& project_root);

// What a session rooted at `session_root` may see: global rows plus its own
// project's rows.
ScopeFilter VisibleFrom(const std::filesystem::path& session_root);

std::unique_ptr<tools::Tool> MakeMemorySearchTool(std::shared_ptr<MemoryRuntime> runtime);
std::unique_ptr<tools::Tool> MakeMemoryGetTool(std::shared_ptr<MemoryRuntime> runtime);
std::unique_ptr<tools::Tool> MakeMemoryStoreTool(std::shared_ptr<MemoryRuntime> runtime);
std::unique_ptr<tools::Tool> MakeMemoryUpdateTool(std::shared_ptr<MemoryRuntime> runtime);
std::unique_ptr<tools::Tool> MakeMemoryDeleteTool(std::shared_ptr<MemoryRuntime> runtime);

// /memory: status, tags maintenance, purge, reindex.
std::unique_ptr<commands::Command> MakeMemoryCommand(std::shared_ptr<MemoryRuntime> runtime);
// /memory when the runtime could not be built: says why, does nothing else.
std::unique_ptr<commands::Command> MakeMemoryUnavailableCommand(std::string reason);

// The "Memory" section pre-filled into each session's context.
std::shared_ptr<agent::ContextContributor> MakeMemoryIndexContributor(
    std::shared_ptr<MemoryRuntime> runtime);
// The same rendering, callable directly (tests, /memory index).
std::string RenderMemoryIndex(MemoryRuntime& runtime, const std::filesystem::path& session_root);

}  // namespace minimal::memory
