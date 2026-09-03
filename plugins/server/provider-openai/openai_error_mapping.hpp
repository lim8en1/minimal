#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "minimal/llm/provider.hpp"
#include "minimal/net/http_client.hpp"

namespace minimal::provider_openai {

// status/body/headers of a completed non-2xx response -> a classified
// LlmError. Pure: no network/curl involved, hence unit-testable with
// synthetic inputs.
llm::LlmError ClassifyHttpFailure(int status, const std::string& body, const std::vector<net::HttpHeader>& headers);

// The payload of an `event: error` SSE frame -> a classified LlmError. Pure.
// These frames are how OpenAI-shaped gateways deliver mid-stream failures;
// they used to be silently swallowed and the turn came back empty.
llm::LlmError ClassifyErrorEventData(std::string_view data);

// Every HttpErrorKind other than Cancelled maps to LlmErrorCategory::Transport.
// Cancelled is deliberately never passed here -- OpenAiProvider intercepts it
// before classification and returns a partial ChatResponse instead.
llm::LlmError ClassifyTransportFailure(const net::HttpError& error);

}  // namespace minimal::provider_openai
