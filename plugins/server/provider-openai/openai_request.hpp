#pragma once

#include <nlohmann/json.hpp>

#include "minimal/llm/chat.hpp"

namespace minimal::provider_openai {

// ChatRequest -> OpenAI /chat/completions request body. Pure, no I/O.
//
// Returns the serialized body directly. The previous implementation built a
// nlohmann DOM of the whole history and dumped it -- two copies of every
// message per provider call -- which is the wrong shape for the 512k-1M token
// goal. Message text is now escaped straight into the output string; only
// small O(1)-sized pieces (tool definitions, tool_choice, `parameters` values)
// go through nlohmann.
//
// Merge order: request.parameters members are spliced in first, minus any key
// that names a structural field; every structural field below is then always
// written from `request`'s own members -- ARCHITECTURE.md's guarantee that a
// misconfigured `parameters` can tweak generation but never corrupt
// conversation/tool-calling mechanics. Dropping colliding parameter keys (not
// emitting them after the structural ones) preserves that guarantee without
// relying on how any given parser resolves duplicate JSON keys.
// `max_tokens` is structural for the same reason even though it is a typed
// ChatRequest member rather than a `parameters` knob.
// `stream` is a parameter, not a read off the request: the provider used to
// deep-copy the whole ChatRequest (tools vector with every schema DOM, the
// parameters object, the system string) per call AND PER RETRY ATTEMPT just
// to flip that one bool. The body is byte-identical across the attempts of
// one tool round; the flag is all that ever differed.
std::string BuildRequestBody(const llm::ChatRequest& request, bool stream);

// Tool definitions and tool_choice are small and structured; nlohmann is the
// right tool for them, and they are exposed individually so each translation
// rule gets its own focused test.
nlohmann::json ToOpenAiTools(const std::vector<llm::ToolDefinition>& tools);
nlohmann::json ToOpenAiToolChoice(const llm::ToolChoiceSpec& choice);

}  // namespace minimal::provider_openai
