#pragma once

// Builds what the provider is sent: the system prompt (preset body + project
// context), the tools array (allowed tools, live tool-source groups, the
// summary tool, preset overrides, spawn/skill strips), the full ChatRequest,
// and the bytes/4 context estimates the compaction trigger uses. Read-only over
// the loop's state -- it writes nothing but its own tool-group list -- so it can
// be reasoned about (and benchmarked) in isolation.
//
// Holds REFERENCES to the loop's preset, config, working history and token
// accumulator: they are members of the owning AgentLoop, which outlives the
// builder and mutates them in place (SetPreset assigns, the history is
// rewritten through WorkingHistory), so the references always see the current
// value. Worker-thread only, like the loop.
//
// The system prompt + tools array is CACHED: it is identical across the rounds
// of a session until the preset, the tool groups or the registry change, and
// rebuilding it (every definition's JSON schema deep-copied, merged and sorted)
// ran two or three times per provider round. The owner invalidates on
// SetPreset/SetToolGroups; registry changes -- which can come from other
// threads (an MCP server reconnecting) -- are caught by its generation().

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "minimal/agent/agent_preset.hpp"
#include "minimal/agent/loop_types.hpp"
#include "minimal/agent/working_history.hpp"
#include "minimal/llm/chat.hpp"
#include "minimal/llm/message.hpp"
#include "minimal/llm/token_accounting.hpp"
#include "minimal/tools/tool_registry.hpp"

namespace minimal::agent {

class RequestBuilder {
public:
    RequestBuilder(const AgentPreset& preset, std::string project_context,
                   tools::ToolRegistry& registry, const LoopConfig& config,
                   const WorkingHistory& working, const llm::TokenAccumulator& tokens,
                   std::function<bool()> is_cancelled, const std::string& session_id)
        : preset_(preset),
          project_context_(std::move(project_context)),
          registry_(registry),
          config_(config),
          working_(working),
          tokens_(tokens),
          is_cancelled_(std::move(is_cancelled)),
          session_id_(session_id) {}

    // Live tool-source groups (e.g. MCP servers) whose tools ride every request.
    void SetToolGroups(std::vector<std::string> groups) {
        tool_groups_ = std::move(groups);
        InvalidateSystemAndTools();
    }

    // The preset changed under the builder's reference (AgentLoop::SetPreset).
    void InvalidateSystemAndTools() { cache_.reset(); }

    // The system prompt and tools array alone -- what the compactor reuses so its
    // request shares the prompt-cache prefix. messages stays out: copying the
    // history just to read these two fields was a RAM spike on a 512k-1M-token
    // session at every compaction check, resume and rewind.
    struct SystemAndTools {
        std::optional<std::string> system;
        std::vector<llm::ToolDefinition> tools;
    };
    // A COPY of the cached value (the request owns its tools array). Callers
    // that only need to read it use CachedSystemAndTools().
    SystemAndTools BuildSystemAndTools() const;
    const SystemAndTools& CachedSystemAndTools() const;

    // The full request. messages BORROWS the history (a span), so the request
    // must not outlive it.
    llm::ChatRequest BuildChatRequest() const;

    // Context size estimate (system + project context + tools + history,
    // bytes/4) built from the same construction BuildChatRequest uses, so the
    // two cannot drift. The two-argument form reuses values the caller already
    // built, avoiding a second tools materialisation and sort.
    std::int64_t EstimateContextTokens() const;
    // The fixed part alone (system + project context + tools): what no
    // compaction can remove.
    std::int64_t FixedContextTokens() const;
    std::int64_t EstimateContextTokens(const std::optional<std::string>& system,
                                       const std::vector<llm::ToolDefinition>& tools) const;

    // The byte ceiling for one tool result: the configured cap, tightened to
    // tools.max_result_window_percent of the model's window (at
    // kConservativeBytesPerToken), so one result can never fill the context on
    // its own. 0 = no ceiling.
    std::int64_t ToolResultByteCap() const;

    // The bytes a tool round's results may add, all together, and still leave
    // the compaction headroom free: the window minus the current context (the
    // larger of the reported occupancy and the estimate) minus
    // kCompactionHeadroom*, converted at kConservativeBytesPerToken -- the
    // new results are unmeasured, and dense output runs ~2 bytes a token.
    // nullopt: no known window, nothing to fit against.
    std::optional<std::int64_t> ToolRoundRoomBytes() const;

    // The room a compaction keeps free on a window (kCompactionHeadroom*).
    static std::int64_t CompactionHeadroomTokens(std::int64_t window);

private:
    SystemAndTools ComputeSystemAndTools() const;
    // system + tools in bytes, the fixed part of every estimate.
    static std::int64_t FixedBytes(const std::optional<std::string>& system,
                                   const std::vector<llm::ToolDefinition>& tools);

    struct Cache {
        SystemAndTools value;
        std::int64_t fixed_bytes = 0;
        std::uint64_t registry_generation = 0;
    };
    // Rebuilt on first use after an invalidation or a registry change.
    const Cache& EnsureCache() const;

    const AgentPreset& preset_;
    std::string project_context_;  // rendered once at session creation; constant after
    tools::ToolRegistry& registry_;
    const LoopConfig& config_;
    const WorkingHistory& working_;
    const llm::TokenAccumulator& tokens_;
    std::function<bool()> is_cancelled_;
    const std::string& session_id_;
    std::vector<std::string> tool_groups_;
    mutable std::optional<Cache> cache_;
};

}  // namespace minimal::agent
