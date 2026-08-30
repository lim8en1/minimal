#include "minimal/agent/compaction_boundary.hpp"

#include <algorithm>
#include <unordered_set>
#include <utility>

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

std::optional<llm::Message> synthesize_missing_tool_results(
    const std::vector<llm::Message>& messages) {
    // The torn tail, when there is one, is always the LAST assistant message:
    // appends are an exact prefix of the turn's writes, so nothing can follow an
    // orphaned tool_use except silence. Searching from the back also means a
    // later, fully-paired tool cycle never masks an earlier problem -- nothing
    // past the trailing assistant message can answer its tool calls.
    std::size_t last_assistant = messages.size();
    for (std::size_t i = messages.size(); i-- > 0;) {
        if (messages[i].role == llm::Role::Assistant) {
            last_assistant = i;
            break;
        }
    }
    if (last_assistant == messages.size()) return std::nullopt;

    std::vector<const llm::ToolUseBlock*> uses;
    for (const auto& block : messages[last_assistant].content) {
        if (const auto* use = std::get_if<llm::ToolUseBlock>(&block)) uses.push_back(use);
    }
    if (uses.empty()) return std::nullopt;

    std::unordered_set<std::string> answered;
    for (std::size_t i = last_assistant + 1; i < messages.size(); ++i) {
        for (const auto& block : messages[i].content) {
            if (const auto* result = std::get_if<llm::ToolResultBlock>(&block)) {
                answered.insert(result->tool_use_id);
            }
        }
    }

    llm::Message repair{llm::Role::User, {}};
    for (const auto* use : uses) {
        if (answered.count(use->id)) continue;
        repair.content.push_back(llm::ToolResultBlock{
            use->id,
            {llm::TextBlock{"Tool execution was interrupted before it completed; "
                            "its result is unavailable. Re-run the call if needed."}},
            true});
    }
    if (repair.content.empty()) return std::nullopt;
    return repair;
}

}  // namespace minimal::agent
