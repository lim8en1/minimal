#include "minimal/agent/request_builder.hpp"

#include <algorithm>

#include "minimal/agent/agent_preset.hpp"
#include "minimal/agent/context_compactor.hpp"
#include "minimal/agent/policy.hpp"
#include "minimal/common/units.hpp"
#include "tool_result_caps.hpp"

namespace minimal::agent {

const RequestBuilder::Cache& RequestBuilder::EnsureCache() const {
    // Read the generation BEFORE building: a registry change racing the build
    // leaves a stale generation behind, so the next call rebuilds instead of
    // trusting a value that may predate the change.
    const std::uint64_t generation = registry_.generation();
    if (!cache_ || cache_->registry_generation != generation) {
        Cache fresh;
        fresh.value = ComputeSystemAndTools();
        fresh.fixed_bytes = FixedBytes(fresh.value.system, fresh.value.tools);
        fresh.registry_generation = generation;
        cache_ = std::move(fresh);
    }
    return *cache_;
}

const RequestBuilder::SystemAndTools& RequestBuilder::CachedSystemAndTools() const {
    return EnsureCache().value;
}

RequestBuilder::SystemAndTools RequestBuilder::BuildSystemAndTools() const {
    return EnsureCache().value;
}

RequestBuilder::SystemAndTools RequestBuilder::ComputeSystemAndTools() const {
    SystemAndTools out;
    if (!preset_.system_prompt.empty()) out.system = preset_.system_prompt;
    // Project context appends AFTER the preset body: both are constant within
    // the session, so the request's prefix -- which the provider's prompt cache
    // keys on -- is stable either way, but appending keeps the preset's own
    // text at the very front across /agent switches.
    if (!project_context_.empty()) {
        if (out.system) *out.system += "\n\n";
        else out.system = std::string();
        *out.system += project_context_;
    }

    std::vector<std::string> allowed = preset_.allowed_tools;
    if (preset_.is_subagent) {
        // Hard rule, not preset config: a subagent never gets the spawn tool, so
        // a misconfigured preset cannot reopen recursion. Subagents are flat,
        // one level deep.
        allowed.erase(std::remove(allowed.begin(), allowed.end(), kSpawnSubagentToolName),
                      allowed.end());
    }
    // Fail-closed spawn allowlist: a preset whose allowed_subagents resolved
    // to an explicit EMPTY list gets no spawn tool, exactly like a preset
    // a definition builder withholds (an empty-named definition) -- advertising a
    // tool every call of which would be refused is worse than offering none.
    // nullopt (the default) means no restriction and reaches this check not
    // at all.
    const bool spawn_allowlist_empty =
        preset_.allowed_subagents.has_value() && preset_.allowed_subagents->empty();
    if (spawn_allowlist_empty) {
        allowed.erase(std::remove(allowed.begin(), allowed.end(), kSpawnSubagentToolName),
                      allowed.end());
    }

    out.tools = registry_.DefinitionsFor(allowed);
    // Live MCP tools for the allowed-and-alive servers, merged into the same
    // sorted order -- this request is rebuilt every turn, so the merge must
    // preserve the deterministic prefix the prompt cache keys on.
    {
        std::vector<llm::ToolDefinition> group_defs = registry_.DefinitionsForGroups(tool_groups_);
        out.tools.insert(out.tools.end(), group_defs.begin(), group_defs.end());
        // Compaction strategies' tools ride every request, whatever the preset
        // allows: a summarizer request reuses this exact tools array (plus one
        // appended instruction), so the tool must already be in it for the
        // prompt cache to cover the history. A stray call outside compaction
        // is answered in RunToolRound, never executed.
        out.tools.insert(out.tools.end(), config_.strategy_tools.begin(),
                         config_.strategy_tools.end());
        std::sort(out.tools.begin(), out.tools.end(),
                  [](const llm::ToolDefinition& a, const llm::ToolDefinition& b) {
                      return a.name < b.name;
                  });
    }
    if (preset_.is_subagent) {
        // DefinitionsFor("*") would have re-admitted it.
        out.tools.erase(std::remove_if(out.tools.begin(), out.tools.end(),
                                       [](const llm::ToolDefinition& def) {
                                           return def.name == kSpawnSubagentToolName;
                                       }),
                        out.tools.end());
    }
    // Preset-scoped definitions replace the static ones name-for-name. They
    // were built once at preset resolve and frozen on the preset, so this is
    // one map lookup per tool and the request prefix -- which the provider's
    // prompt cache keys on -- never shifts mid-session. Run AFTER the merge
    // and sort so the final array is what gets rewritten; before the strips
    // below, which operate on the same names.
    for (auto& def : out.tools) {
        const auto it = preset_.tool_definition_overrides.find(def.name);
        if (it != preset_.tool_definition_overrides.end()) def = it->second;
    }
    // A builder that withholds its tool from this preset answers an
    // EMPTY-named definition (the skill tool with no skill allowed): the tool
    // would only ever return errors, so the request leaves it out.
    out.tools.erase(std::remove_if(out.tools.begin(), out.tools.end(),
                                   [](const llm::ToolDefinition& def) { return def.name.empty(); }),
                    out.tools.end());
    return out;
}

llm::ChatRequest RequestBuilder::BuildChatRequest() const {
    llm::ChatRequest request;
    const Cache& cache = EnsureCache();
    // The larger of the fresh estimate and the last backend-reported
    // occupancy: the estimate can undercount, the occupancy lags a round.
    const std::int64_t input_tokens =
        std::max(cache.fixed_bytes / 4 + working_.estimated_tokens(), tokens_.last_occupancy());
    request.system = cache.value.system;
    request.tools = cache.value.tools;
    request.messages = working_.messages();
    request.should_abort = is_cancelled_;
    request.model = preset_.provider.model_name;
    request.parameters = preset_.parameters;
    request.max_tokens = FitOutputCap(preset_.max_tokens, preset_.context_window_tokens, input_tokens);
    // The harness session id, riding every request this loop builds -- the
    // opt-out (parameters "session_id": false) is the provider builder's
    // decision, made per request from the parameters riding alongside.
    request.session_id = session_id_;
    request.stream = true;
    return request;
}

// The estimate is built FROM the same cached system/tools BuildChatRequest
// uses -- system prompt, project context and tools array included -- rather
// than alongside it, so the two cannot drift: a /agent switch, a
// project-context load or an allowed-tools change invalidate the one cache
// both read. The history half is WorkingHistory's running count, so neither
// half walks anything per call. The bytes/4 roughness matches
// estimate_tokens, which is deliberate: the first real response replaces the
// estimate with the backend's ground truth anyway.
std::int64_t RequestBuilder::EstimateContextTokens() const {
    return EnsureCache().fixed_bytes / 4 + working_.estimated_tokens();
}

std::int64_t RequestBuilder::FixedContextTokens() const { return EnsureCache().fixed_bytes / 4; }

std::int64_t RequestBuilder::EstimateContextTokens(const std::optional<std::string>& system,
                                              const std::vector<llm::ToolDefinition>& tools) const {
    return FixedBytes(system, tools) / 4 + working_.estimated_tokens();
}

std::int64_t RequestBuilder::FixedBytes(const std::optional<std::string>& system,
                                        const std::vector<llm::ToolDefinition>& tools) {
    std::int64_t bytes = system ? static_cast<std::int64_t>(system->size()) : 0;
    for (const auto& def : tools) {
        bytes += static_cast<std::int64_t>(def.name.size() + def.description.size());
        bytes += JsonSizeEstimate(def.input_schema);
    }
    return bytes;
}

std::int64_t RequestBuilder::CompactionHeadroomTokens(std::int64_t window) {
    return std::max(kCompactionHeadroomMinTokens,
                    common::PercentOf(window, kCompactionHeadroomPercent));
}

std::optional<std::int64_t> RequestBuilder::ToolRoundRoomBytes() const {
    if (!preset_.context_window_tokens || *preset_.context_window_tokens <= 0) return std::nullopt;
    const std::int64_t window = *preset_.context_window_tokens;
    const std::int64_t occupancy = std::max(EstimateContextTokens(), tokens_.last_occupancy());
    const std::int64_t room = window - occupancy - CompactionHeadroomTokens(window);
    return std::max<std::int64_t>(room, 0) * kConservativeBytesPerToken;
}

std::int64_t RequestBuilder::ToolResultByteCap() const {
    std::int64_t cap = config_.max_tool_result_bytes;
    // The window term is a share of the window converted to BYTES at the
    // conservative ratio -- the window itself is in tokens, and using it as a
    // byte count let one result take the whole window or more.
    if (config_.max_tool_result_window_percent > 0 && preset_.context_window_tokens &&
        *preset_.context_window_tokens > 0) {
        const std::int64_t window_cap = std::max<std::int64_t>(
            common::PercentOf(*preset_.context_window_tokens,
                              config_.max_tool_result_window_percent) *
                kConservativeBytesPerToken,
            kMinToolResultCapBytes);
        cap = cap > 0 ? std::min(cap, window_cap) : window_cap;
    }
    return cap;
}

}  // namespace minimal::agent
