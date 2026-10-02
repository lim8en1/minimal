#include "minimal/agent/transcript_repair.hpp"

#include <unordered_set>

namespace minimal::agent {

std::optional<llm::Message> synthesize_missing_tool_results(
    const std::vector<llm::Message>& messages) {
    // RESUME ONLY, for transcripts written before a tool round was committed
    // as one atomic unit (HistoryStore::AppendRound): the live loop can no
    // longer produce a torn tail. The tail, when torn, is the LAST assistant
    // message, so searching from the back means a later, fully-paired tool
    // cycle never masks an earlier problem.
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
