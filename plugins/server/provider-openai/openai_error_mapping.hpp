#pragma once

#include <string>
#include <vector>

#include "minimal/llm/http_client.hpp"
#include "minimal/llm/provider.hpp"

namespace minimal::provider_openai {

// status/body/headers of a completed non-2xx response -> a classified
// LlmError. Pure: no network/curl involved, hence unit-testable with
// synthetic inputs.
llm::LlmError ClassifyHttpFailure(int status, const std::string& body, const std::vector<llm::HttpHeader>& headers);

// Every HttpErrorKind other than Cancelled maps to LlmErrorCategory::Transport.
// Cancelled is deliberately never passed here -- OpenAiProvider intercepts it
// before classification and returns a partial ChatResponse instead.
llm::LlmError ClassifyTransportFailure(const llm::HttpError& error);

}  // namespace minimal::provider_openai
