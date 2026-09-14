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
//
// PROMPT CACHING (opt-in): `parameters.cache_control = true` (consumed here,
// never emitted verbatim -- it names a StructuralKey) switches the serializer
// to OpenRouter's Anthropic-style form, which is how Anthropic-model caching is
// reachable through OpenAI-shaped endpoints: `system` becomes a content-part
// array carrying a `cache_control` breakpoint (tools render before system on
// the wire, so this one marker caches tools + system together), and the LAST
// wire message's content becomes a marked part array too -- the growing
// conversation tail, auto-advanced by the walk the same way provider-anthropic
// places its top-level breakpoint. `parameters.cache_ttl` ("5m" default, or
// "1h") sets the breakpoints' ttl, the same key provider-anthropic consumes.
// Flag off -- the default -- leaves the body byte-identical to the historical
// form: plain OpenAI, llama.cpp and vLLM cache prompts without markers (or
// reject unknown content-part fields), and a marked message would only ever
// reach those backends by misconfiguration. The tail message's `tool_calls`
// and assistant null-content cases carry no breakpoint (there is no text part
// to hang it on); the system breakpoint still covers the prefix there.
// max_tokens_field: the JSON key the output cap is emitted under -- the
// provider entry's "max_tokens_field" ("max_tokens" when empty, the
// historical spelling llama.cpp/vLLM accept; "max_completion_tokens" for
// newer OpenAI models). The active cap field is also DROPPED from
// `parameters` alongside the fixed structural keys, so the backend never
// sees two competing caps.
std::string BuildRequestBody(const llm::ChatRequest& request, bool stream,
                             const std::string& max_tokens_field = "max_tokens");

// SESSION ROUTING (default-on): the effective top-level `session_id` body
// value for the request ("" = omit). OpenRouter uses the field as its
// provider-sticky-routing key -- the whole session lands on the endpoint
// whose prompt cache is warm, and the requests group in its Logs "Sessions"
// view; backends that ignore unknown body fields are unaffected. The value
// rides llm::ChatRequest::session_id (set by the agent loop from the harness
// session id, compaction summarizer included). A boolean `parameters`
// "session_id" is CONSUMED (true keeps the dynamic value, false opts out for
// backends that reject unknown body fields); a STRING value is a static
// routing key that passes through the parameters loop verbatim and suppresses
// the dynamic one, so the wire never carries duplicate keys. Exposed because
// BuildRequestBody's output is asserted through it and the decision deserves
// its own focused tests, like ToOpenAiTools above.
std::string SessionIdRoutingValue(const llm::ChatRequest& request);

// Tool definitions and tool_choice are small and structured; nlohmann is the
// right tool for them, and they are exposed individually so each translation
// rule gets its own focused test.
nlohmann::json ToOpenAiTools(const std::vector<llm::ToolDefinition>& tools);
nlohmann::json ToOpenAiToolChoice(const llm::ToolChoiceSpec& choice);

}  // namespace minimal::provider_openai
