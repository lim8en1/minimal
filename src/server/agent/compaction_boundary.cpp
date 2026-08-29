#include "minimal/agent/compaction_boundary.hpp"

#include <algorithm>
#include <unordered_set>

namespace minimal::agent {

std::size_t snap_compaction_boundary(const std::vector<llm::Message>& messages,
                                      std::size_t candidate_cut_index) {
    std::size_t boundary = std::min(candidate_cut_index, messages.size());

    while (boundary > 0) {
        std::unordered_set<std::string> pending_ids;
        for (std::size_t i = 0; i < boundary; ++i) {
            for (const auto& block : messages[i].content) {
                if (const auto* tool_use = std::get_if<llm::ToolUseBlock>(&block)) {
                    pending_ids.insert(tool_use->id);
                }
            }
        }
        if (pending_ids.empty()) break;  // no tool calls in the compacted region at all

        for (std::size_t i = 0; i < boundary; ++i) {
            for (const auto& block : messages[i].content) {
                if (const auto* tool_result = std::get_if<llm::ToolResultBlock>(&block)) {
                    pending_ids.erase(tool_result->tool_use_id);
                }
            }
        }
        if (pending_ids.empty()) break;  // every tool_use paired within the region

        --boundary;  // an orphaned tool_use -- shrink the region by one and retry
    }
    return boundary;
}

}  // namespace minimal::agent
