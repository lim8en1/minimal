#pragma once

#include <cstddef>
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

}  // namespace minimal::agent
