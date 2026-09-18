#pragma once

// The four tools + the /memory command. Construction and wiring:
// MemoryTools holds the shared state (store, embedder, enricher) behind the
// plugin's static holder; each tool is a thin Tool impl over it.

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>

#include "enricher.hpp"
#include "embedding.hpp"
#include "memory_settings.hpp"
#include "memory_store.hpp"

#include "minimal/commands/command.hpp"
#include "minimal/net/http_client.hpp"

namespace minimal::plugins {
class ServiceRegistry;
}
namespace minimal::tools {
class Tool;
}

namespace minimal::memory {

// The plugin's shared state: one store, one embedder (may be null = lexical
// only), one enricher (may be inert). Owned by the plugin's static holder so
// tools, the /memory command and the re-embed drain share one instance.
struct MemoryRuntime {
    MemorySettings settings;
    std::unique_ptr<MemoryStore> store;
    std::unique_ptr<EmbeddingFunction> embedder;  // null = no embeddings
    std::unique_ptr<Enricher> enricher;           // non-null; inert when disabled
    std::string signature;                        // current strategy signature
    plugins::ServiceRegistry* services = nullptr; // enrichment model resolution

    // Digest memoization: first memory-tool call of a session prepends it.
    std::mutex digest_mu;
    std::unordered_set<std::string> digest_shown;
};

// Worktree-root walk shared by the tools' scope=project resolution and the
// /memory command's project-topics listing (memory_tool.cpp).
std::filesystem::path WorktreeRoot(const std::filesystem::path& project_root);

std::unique_ptr<tools::Tool> MakeMemorySearchTool(std::shared_ptr<MemoryRuntime> runtime);
std::unique_ptr<tools::Tool> MakeMemoryStoreTool(std::shared_ptr<MemoryRuntime> runtime);
std::unique_ptr<tools::Tool> MakeMemoryGetTool(std::shared_ptr<MemoryRuntime> runtime);
std::unique_ptr<tools::Tool> MakeMemoryDeleteTool(std::shared_ptr<MemoryRuntime> runtime);

// /memory -- the operator view of the same digest the model sees (counts,
// by-origin, topics incl. zero-entry ones, backend + enrichment state,
// pending reindex) plus `purge <path>` for bulk group removal.
std::unique_ptr<commands::Command> MakeMemoryCommand(std::shared_ptr<MemoryRuntime> runtime);

// sidecar backend (embedding_sidecar.cpp): spawns a long-lived python/
// fastembed subprocess; null on any failure (python missing, spawn failure,
// fastembed import/model-load failure) -- strategy degrades, the plugin
// still loads.
std::unique_ptr<EmbeddingFunction> TryMakeSidecarEmbeddingFunction(
    const MemorySettings& settings, const std::filesystem::path& data_dir);

}  // namespace minimal::memory
