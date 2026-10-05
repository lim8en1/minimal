#include "minimal/agent/compaction_strategy.hpp"

#include <algorithm>
#include <span>

namespace minimal::agent {

bool CompactionStrategyRegistry::Register(std::string kind, CompactionStrategy strategy) {
    if (!strategy.cut_selector || !strategy.summarize) return false;
    return AttributedRegistry::Register(std::move(kind), std::move(strategy));
}

std::vector<llm::ToolDefinition> CompactionStrategyRegistry::RequestTools() const {
    std::vector<llm::ToolDefinition> out;
    for (const auto& kind : kinds()) {
        for (const auto& tool : Find(kind)->request_tools) {
            const bool seen = std::any_of(out.begin(), out.end(), [&](const llm::ToolDefinition& t) {
                return t.name == tool.name;
            });
            if (!seen) out.push_back(tool);
        }
    }
    return out;
}

std::map<std::string, std::string> CompactionStrategyRegistry::StrayCallReplies() const {
    std::map<std::string, std::string> out;
    for (const auto& kind : kinds()) {
        const auto* strategy = Find(kind);
        for (const auto& tool : strategy->request_tools) {
            out.emplace(tool.name, strategy->stray_call_reply);
        }
    }
    return out;
}

std::size_t ProposeSummarizeCut(const std::vector<llm::Message>& history,
                                std::int64_t keep_recent_tokens) {
    if (keep_recent_tokens <= 0) return history.size() / 2;  // no budget: cut by count

    // Walk from the tail, accumulating PER-MESSAGE estimates, until the
    // budget would be exceeded. Each message is its own unit -- a giant
    // recent tool result either fits under the budget whole or pushes the
    // cut to just before it, never split mid-message (the same "never
    // orphan a tool call" spirit snap_compaction_boundary enforces
    // structurally; this is the size-side analogue).
    //
    // Computed ON DEMAND here rather than maintained incrementally alongside
    // history_ (W11's original design) -- compaction is infrequent, so an
    // O(n) walk at decision time costs nothing a session would ever notice,
    // and it needs no new state threaded through every history mutation
    // (TruncateAfter, ReseedFromStore, RestorePersisted, the fold itself).
    std::int64_t accumulated = 0;
    std::size_t idx = history.size();
    while (idx > 0) {
        const std::span<const llm::Message> one(&history[idx - 1], 1);
        const std::int64_t message_tokens = estimate_tokens(one);
        if (accumulated > 0 && accumulated + message_tokens > keep_recent_tokens) break;
        accumulated += message_tokens;
        --idx;
    }
    return idx;
}

CompactionStrategy MakeSummarizeCompactionStrategy() {
    CompactionStrategy strategy;
    strategy.cut_selector = ProposeSummarizeCut;
    strategy.summarize = compact_messages;
    return strategy;
}

}  // namespace minimal::agent
