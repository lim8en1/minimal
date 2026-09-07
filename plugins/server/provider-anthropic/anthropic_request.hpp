#pragma once

#include <nlohmann/json.hpp>

#include "minimal/llm/chat.hpp"

namespace minimal::provider_anthropic {

// ChatRequest -> Anthropic Messages API request body. Pure, no I/O.
//
// Serialized incrementally into a plain string, same rationale as
// provider-openai's BuildRequestBody: the history dominates the body and
// this project's stated goal is 512k-1M token histories, so a DOM of the
// whole thing (and a second one on dump()) is the wrong shape. Only
// O(1)-sized pieces (tool schemas, `parameters` values) go through nlohmann.
//
// CONSECUTIVE SAME-ROLE MESSAGES ARE MERGED. Anthropic's Messages API
// enforces strict user/assistant alternation and refuses two consecutive
// messages of the same role -- unlike OpenAI, which has no such rule.
// mention_expansion.hpp already documents this constraint (multi-block
// content in ONE message, specifically to avoid it); this builder is the
// second place it matters, since a compaction summary or an injected
// message can otherwise land next to another message of the same role.
// Content blocks from adjacent same-role llm::Messages are folded into one
// Anthropic message's content array rather than emitted as separate wire
// messages.
//
// SYSTEM AND CACHING: `system` is always emitted as a one-element block
// array (not a bare string) with an explicit `cache_control` breakpoint on
// it when ChatRequest::system is set -- tools render before system on the
// wire, so this one marker caches tools + system together (the
// session-constant, expensive part of the prefix). A top-level
// `cache_control` is also always attached to the request body, which
// Anthropic auto-places on the last cacheable block of the growing message
// tail -- the documented "robust combination for agent loops" (two of the
// four allowed breakpoints). `cache_ttl` ("5m", the default, or "1h") is
// read from `parameters.cache_ttl` and consumed there: it is not a real
// Anthropic request field, so it never reaches the output verbatim.
//
// `max_tokens` is REQUIRED by the Anthropic API, unlike ChatRequest's
// optional member. providers.json already lets an operator default it per
// -provider or per-model (see providers_config.hpp's "Provider-level
// values are defaults for every model"), so this throws
// std::invalid_argument only when NEITHER is configured -- the same
// validate-in-the-builder shape as provider-openai's ToOpenAiToolChoice.
//
// THINKING BLOCKS ARE DROPPED ON REPLAY. Populating them requires the
// `signature` field llm::ThinkingBlock declares but never populates end to
// end (message.hpp marks it "reserved") -- sending an unsigned or
// resynthesized thinking block back to Anthropic is either rejected or, on
// models with preserved-thinking history-editing checks, silently wrong.
// This is cut 1's known limitation, not an oversight.
std::string BuildRequestBody(const llm::ChatRequest& request, bool stream);

// Tool definitions and tool_choice are small and structured; nlohmann is the
// right tool for them, exposed individually so each translation rule gets
// its own focused test -- same split as provider-openai's equivalents.
nlohmann::json ToAnthropicTools(const std::vector<llm::ToolDefinition>& tools);
nlohmann::json ToAnthropicToolChoice(const llm::ToolChoiceSpec& choice);

}  // namespace minimal::provider_anthropic
