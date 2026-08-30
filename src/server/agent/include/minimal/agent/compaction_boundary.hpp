#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "minimal/llm/message.hpp"

namespace minimal::agent {

// Given a candidate cut index into `messages` (messages before the index would
// be summarized by compaction; messages at/after it stay verbatim), snap the
// boundary backward to the nearest index that does not separate an assistant
// ToolUseBlock from its ToolResultBlock -- cutting between the two is a hard 400
// on both OpenAI and Anthropic, not a soft degradation. A boundary with no
// pending tool calls at all needs no snap and is returned unchanged.
std::size_t snap_compaction_boundary(const std::vector<llm::Message>& messages,
                                      std::size_t candidate_cut_index);

// Repair a transcript that ends mid-tool-sequence, so it can be replayed to a
// provider on resume.
//
// A session cancelled mid-tool-execution (or a process killed between the
// assistant append and the tool-results append) persists a tail whose last
// assistant message carries ToolUseBlocks with no matching ToolResultBlock.
// Sending that to a provider is a hard 400 on both wire formats -- the same
// pairing rule snap_compaction_boundary protects. This returns a User message
// holding one error ToolResultBlock per UNMATCHED tool_use of the trailing
// assistant message, or nullopt when the history already ends provider-valid
// (no trailing assistant message, or every tool_use answered). The repair is
// a real new message, appended and persisted on resume, never a rewrite of the
// stored tail.
std::optional<llm::Message> synthesize_missing_tool_results(
    const std::vector<llm::Message>& messages);

}  // namespace minimal::agent
