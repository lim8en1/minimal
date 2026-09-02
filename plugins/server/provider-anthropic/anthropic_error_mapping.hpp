#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "minimal/llm/http_client.hpp"
#include "minimal/llm/provider.hpp"

namespace minimal::provider_anthropic {

// status/body/headers of a completed non-2xx response -> a classified
// LlmError. Pure: no network/curl involved, hence unit-testable with
// synthetic inputs. Classifies primarily by the response body's own
// `error.type` field (Anthropic always names it explicitly), falling back
// to the HTTP status when the body isn't the expected JSON shape.
llm::LlmError ClassifyHttpFailure(int status, const std::string& body,
                                  const std::vector<llm::HttpHeader>& headers);

// The payload of an `event: error` SSE frame -> a classified LlmError. Pure.
// Same shape and same reason provider-openai has one: a mid-stream failure
// must not be silently swallowed by the reassembler.
llm::LlmError ClassifyErrorEventData(std::string_view data);

// Every HttpErrorKind other than Cancelled maps to LlmErrorCategory::Transport.
// Cancelled is deliberately never passed here -- AnthropicProvider intercepts
// it before classification and returns a partial ChatResponse instead.
llm::LlmError ClassifyTransportFailure(const llm::HttpError& error);

}  // namespace minimal::provider_anthropic
