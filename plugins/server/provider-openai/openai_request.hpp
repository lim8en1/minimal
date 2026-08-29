#pragma once

#include <nlohmann/json.hpp>

#include "minimal/llm/chat.hpp"

namespace minimal::provider_openai {

// ChatRequest -> OpenAI /chat/completions request body. Pure, no I/O.
//
// Merge order: request.parameters is copied in first, then every structural
// field below is unconditionally (re)written from `request`'s own members --
// ARCHITECTURE.md's guarantee that a misconfigured `parameters` can tweak
// generation but never corrupt conversation/tool-calling mechanics.
// `max_tokens` is treated as structural for the same reason even though it's
// a typed ChatRequest member rather than a `parameters` knob.
nlohmann::json BuildRequestBody(const llm::ChatRequest& request);

// Exposed individually so each translation rule gets its own focused test.
nlohmann::json ToOpenAiMessages(const llm::ChatRequest& request);
nlohmann::json ToOpenAiTools(const std::vector<llm::ToolDefinition>& tools);
nlohmann::json ToOpenAiToolChoice(const llm::ToolChoiceSpec& choice);

}  // namespace minimal::provider_openai
