#include "minimal/agent/compaction_strategy.hpp"

#include <algorithm>
#include <span>

namespace minimal::agent {

bool CompactionStrategyRegistry::Register(std::string kind, CompactionStrategy strategy) {
    if (!strategy.cut_selector || !strategy.summarize) return false;
    return strategies_.emplace(std::move(kind), std::move(strategy)).second;
}

const CompactionStrategy* CompactionStrategyRegistry::Find(std::string_view kind) const {
    const auto it = strategies_.find(kind);
    return it == strategies_.end() ? nullptr : &it->second;
}

bool CompactionStrategyRegistry::Has(std::string_view kind) const {
    return strategies_.find(kind) != strategies_.end();
}

std::vector<std::string> CompactionStrategyRegistry::kinds() const {
    std::vector<std::string> out;
    out.reserve(strategies_.size());
    for (const auto& [kind, _] : strategies_) out.push_back(kind);
    return out;  // map iteration is already sorted
}

std::size_t ProposeSummarizeCut(const std::vector<llm::Message>& history,
                                std::int64_t keep_recent_tokens) {
    if (keep_recent_tokens <= 0) return history.size() / 2;  // legacy default

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
