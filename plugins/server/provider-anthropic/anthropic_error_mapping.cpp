#include "anthropic_error_mapping.hpp"

#include <chrono>
#include <optional>

#include <nlohmann/json.hpp>

namespace minimal::provider_anthropic {

namespace {

// Anthropic's error body is always {"type":"error","error":{"type":...,
// "message":...}}. The inner `error.type` is a closed, documented set, so
// classifying by it is more precise than guessing from the HTTP status --
// unlike provider-openai, which has to string-sniff an SSE error's message
// text because OpenAI-shaped gateways don't standardize an error-type field.
struct ParsedError {
    bool ok = false;
    std::string type;
    std::string message;
};

ParsedError ParseErrorBody(const std::string& body) {
    ParsedError parsed;
    try {
        auto json = nlohmann::json::parse(body);
        if (!json.contains("error")) return parsed;
        const auto& error = json.at("error");
        parsed.type = error.value("type", "");
        parsed.message = error.value("message", "");
        parsed.ok = true;
    } catch (const nlohmann::json::exception&) {
        // Not JSON, or not the expected shape -- caller falls back to status.
    }
    return parsed;
}

llm::LlmErrorCategory CategoryForErrorType(const std::string& type) {
    if (type == "rate_limit_error") return llm::LlmErrorCategory::RateLimit;
    if (type == "authentication_error" || type == "permission_error") {
        return llm::LlmErrorCategory::AuthError;
    }
    if (type == "overloaded_error" || type == "api_error") return llm::LlmErrorCategory::ServerError;
    // "invalid_request_error", "not_found_error", "request_too_large", and
    // anything not yet documented: conservative default, non-retryable --
    // hammering an endpoint that already said the request itself is bad
    // helps nobody.
    return llm::LlmErrorCategory::RequestError;
}

llm::LlmErrorCategory CategoryForStatus(int status) {
    if (status == 429) return llm::LlmErrorCategory::RateLimit;
    if (status == 401 || status == 403) return llm::LlmErrorCategory::AuthError;
    if (status == 408) return llm::LlmErrorCategory::Transport;   // gateway timeout analogue
    if (status >= 500) return llm::LlmErrorCategory::ServerError;
    return llm::LlmErrorCategory::RequestError;
}

// Standard "Retry-After" only (seconds) -- unlike OpenAI, Anthropic has no
// documented millisecond-precision variant to prefer. Hostile or absurd
// values (negative, overflow, an HTTP-date) leave the hint unset rather than
// turning into a wild wait, same discipline as provider-openai's parser.
std::optional<std::chrono::milliseconds> ParseRetryAfter(
    const std::vector<llm::HttpHeader>& headers) {
    const auto value = llm::FindHeaderCI(headers, "Retry-After");
    if (!value) return std::nullopt;
    try {
        std::size_t consumed = 0;
        const long long seconds = std::stoll(*value, &consumed);
        if (seconds <= 0 || consumed != value->size()) return std::nullopt;
        const auto duration = std::chrono::seconds(seconds);
        if (duration > std::chrono::minutes(10)) return std::nullopt;
        return std::chrono::duration_cast<std::chrono::milliseconds>(duration);
    } catch (const std::exception&) {
        return std::nullopt;  // non-numeric (e.g. an HTTP-date) -- leave unset
    }
}

}  // namespace

llm::LlmError ClassifyHttpFailure(int status, const std::string& body,
                                  const std::vector<llm::HttpHeader>& headers) {
    const ParsedError parsed = ParseErrorBody(body);
    const std::optional<std::chrono::milliseconds> retry_after = ParseRetryAfter(headers);

    const llm::LlmErrorCategory category =
        parsed.ok && !parsed.type.empty() ? CategoryForErrorType(parsed.type) : CategoryForStatus(status);
    const std::string message =
        parsed.ok && !parsed.message.empty() ? parsed.message : ("HTTP " + std::to_string(status));
    return llm::LlmError(category, message, status, retry_after);
}

llm::LlmError ClassifyErrorEventData(std::string_view data) {
    // The payload of an `event: error` SSE frame -- how Anthropic delivers a
    // mid-stream failure (most commonly overloaded_error). No HTTP status or
    // headers travel with it, so there is no Retry-After to parse here.
    const ParsedError parsed = ParseErrorBody(std::string(data));
    const llm::LlmErrorCategory category =
        parsed.ok && !parsed.type.empty() ? CategoryForErrorType(parsed.type)
                                          : llm::LlmErrorCategory::ServerError;
    const std::string message = parsed.ok && !parsed.message.empty() ? parsed.message : std::string(data);
    return llm::LlmError(category, message);
}

llm::LlmError ClassifyTransportFailure(const llm::HttpError& error) {
    return llm::LlmError(llm::LlmErrorCategory::Transport, error.what());
}

}  // namespace minimal::provider_anthropic
