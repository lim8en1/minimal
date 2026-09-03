#include "minimal/agent/compaction_boundary.hpp"

#include <algorithm>
#include <string>
#include <unordered_map>

namespace minimal::agent {

std::size_t snap_compaction_boundary(const std::vector<llm::Message>& messages,
                                      std::size_t candidate_cut_index) {
    std::size_t boundary = std::min(candidate_cut_index, messages.size());

    // INCREMENTAL, not a rescan per shrink. The naive version rebuilt the
    // pending-uses set for the whole region on every one-message shrink --
    // O(cut) per step, O(cut^2) worst case, on the path every compaction pays.
    // Instead: one pass builds per-id use/result counts for the full candidate
    // region, and each shrink only un-counts the message that left it.
    //
    // A region is safe when no id has more uses than results inside it
    // (imbalanced == 0): every tool_use in the region is answered by a result
    // in the region, which is the pairing the providers require.
    struct Counts {
        int uses = 0;
        int results = 0;
    };
    std::unordered_map<std::string, Counts> counts;
    std::size_t imbalanced = 0;  // ids where uses > results

    const auto adjust = [&](const std::string& id, const int d_use, const int d_result) {
        auto& c = counts[id];
        const bool was = c.uses > c.results;
        c.uses += d_use;
        c.results += d_result;
        const bool now = c.uses > c.results;
        imbalanced += (now ? 1 : 0) - (was ? 1 : 0);
    };

    for (std::size_t i = 0; i < boundary; ++i) {
        for (const auto& block : messages[i].content) {
            if (const auto* tool_use = std::get_if<llm::ToolUseBlock>(&block)) {
                adjust(tool_use->id, +1, 0);
            } else if (const auto* tool_result = std::get_if<llm::ToolResultBlock>(&block)) {
                adjust(tool_result->tool_use_id, 0, +1);
            }
        }
    }

    while (boundary > 0 && imbalanced > 0) {
        --boundary;  // an orphaned tool_use -- shrink the region by one and retry
        for (const auto& block : messages[boundary].content) {
            if (const auto* tool_use = std::get_if<llm::ToolUseBlock>(&block)) {
                adjust(tool_use->id, -1, 0);
            } else if (const auto* tool_result = std::get_if<llm::ToolResultBlock>(&block)) {
                adjust(tool_result->tool_use_id, 0, -1);
            }
        }
    }
    return boundary;
}

}  // namespace minimal::agent
