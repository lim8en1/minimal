#pragma once

// Builds what the provider is sent: the system prompt (preset body + project
// context), the tools array (allowed tools, live tool-source groups, the
// summary tool, preset overrides, spawn/skill strips), the full ChatRequest,
// and the bytes/4 context estimates the compaction trigger uses. Read-only over
// the loop's state -- it writes nothing but its own tool-group list -- so it can
// be reasoned about (and benchmarked) in isolation.
//
// Holds REFERENCES to the loop's preset, config, history and token accumulator:
// they are members of the owning AgentLoop, which outlives the builder and
// mutates them in place (SetPreset assigns, history_ is reassigned wholesale),
// so the references always see the current value. Worker-thread only, like the
// loop.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "minimal/agent/agent_preset.hpp"
#include "minimal/agent/loop_types.hpp"
#include "minimal/llm/chat.hpp"
#include "minimal/llm/message.hpp"
#include "minimal/llm/token_accounting.hpp"
#include "minimal/tools/tool_registry.hpp"

namespace minimal::agent {

class RequestBuilder {
public:
    RequestBuilder(const AgentPreset& preset, std::string project_context,
                   tools::ToolRegistry& registry, const LoopConfig& config,
                   const std::vector<llm::Message>& history, const llm::TokenAccumulator& tokens,
                   std::function<bool()> is_cancelled, const std::string& session_id)
        : preset_(preset),
          project_context_(std::move(project_context)),
          registry_(registry),
          config_(config),
          history_(history),
          tokens_(tokens),
          is_cancelled_(std::move(is_cancelled)),
          session_id_(session_id) {}

    // Live tool-source groups (e.g. MCP servers) whose tools ride every request.
    void SetToolGroups(std::vector<std::string> groups) { tool_groups_ = std::move(groups); }

    // The system prompt and tools array alone -- what the compactor reuses so its
    // request shares the prompt-cache prefix. messages stays out: copying the
    // history just to read these two fields was a RAM spike on a 512k-1M-token
    // session at every compaction check, resume and rewind.
    struct SystemAndTools {
        std::optional<std::string> system;
        std::vector<llm::ToolDefinition> tools;
    };
    SystemAndTools BuildSystemAndTools() const;

    // The full request. messages BORROWS the history (a span), so the request
    // must not outlive it.
    llm::ChatRequest BuildChatRequest() const;

    // Context size estimate (system + project context + tools + history,
    // bytes/4) built from the same construction BuildChatRequest uses, so the
    // two cannot drift. The two-argument form reuses values the caller already
    // built, avoiding a second tools materialisation and sort.
    std::int64_t EstimateContextTokens() const;
    std::int64_t EstimateContextTokens(const std::optional<std::string>& system,
                                       const std::vector<llm::ToolDefinition>& tools) const;

    // The byte ceiling for one tool result: the configured cap, tightened to
    // what a quarter of the model's window can hold (about 4 bytes a token), so
    // one result can never fill the context on its own. 0 = no ceiling.
    std::int64_t ToolResultByteCap() const;

private:
    const AgentPreset& preset_;
    std::string project_context_;  // rendered once at session creation; constant after
    tools::ToolRegistry& registry_;
    const LoopConfig& config_;
    const std::vector<llm::Message>& history_;
    const llm::TokenAccumulator& tokens_;
    std::function<bool()> is_cancelled_;
    const std::string& session_id_;
    std::vector<std::string> tool_groups_;
};

}  // namespace minimal::agent
