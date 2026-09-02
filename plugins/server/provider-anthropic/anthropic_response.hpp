#pragma once

#include <stdexcept>

#include <nlohmann/json.hpp>

#include "minimal/llm/chat.hpp"

namespace minimal::provider_anthropic {

// Thrown on a malformed/unexpected JSON shape from either the non-streaming
// parser or the stream reassembler. Kept distinct from llm::LlmError so this
// header stays independent of the provider interface -- same split as
// provider-openai's OpenAiParseError. AnthropicProvider's glue catches this
// and rethrows as LlmError(ParseError, ...).
struct AnthropicParseError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

llm::StopReason MapStopReason(const std::string& stop_reason);
llm::Usage ParseUsage(const nlohmann::json& usage_json);

// Full non-streaming Messages API response body.
llm::ChatResponse ParseNonStreamingResponse(const nlohmann::json& body);

}  // namespace minimal::provider_anthropic
