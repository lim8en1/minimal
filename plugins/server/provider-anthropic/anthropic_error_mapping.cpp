#include "anthropic_error_mapping.hpp"

#include <chrono>
#include <optional>

#include <nlohmann/json.hpp>
#include "minimal/common/units.hpp"
#include "minimal/common/http_status.hpp"

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

// Only the head of an error body is ever parsed. The body can be as large as
// the HTTP client's caps (net::kMaxResponseBodyBytes for a GET,
// net::kMaxPostBodyBytes for a completion -- net/policy.hpp), so handing all of it to nlohmann to
// extract one string is a self-inflicted stall on a hostile (or merely
// absurd) error body. Same bound as provider-openai's HeadOf.
constexpr std::size_t kMaxErrorBodyParse = 64 * common::kKiB;

std::string_view HeadOf(const std::string& body) {
    std::string_view slice(body);
    if (slice.size() > kMaxErrorBodyParse) slice = slice.substr(0, kMaxErrorBodyParse);
    return slice;
}

ParsedError ParseErrorBody(const std::string& body) {
    // The extracted message is server-authored; cap it too so no
    // classification path can embed more than this into an exception.
    constexpr std::size_t kMaxMessage = 2000;
    ParsedError parsed;
    try {
        auto json = nlohmann::json::parse(HeadOf(body));
        if (!json.contains("error")) return parsed;
        const auto& error = json.at("error");
        parsed.type = error.value("type", "");
        parsed.message = error.value("message", "");
        if (parsed.message.size() > kMaxMessage) {
            parsed.message.resize(kMaxMessage);
            parsed.message += " …[truncated]";
        }
        parsed.ok = true;
    } catch (const nlohmann::json::exception&) {
        // Not JSON, or not the expected shape (including a slice truncated
        // mid-document) -- caller falls back to status.
    }
    return parsed;
}

// nullopt = the type is not in Anthropic's documented set. The caller must
// then fall back to the HTTP status rather than the conservative
// non-retryable default this used to return: a gateway that answers 503 with
// a body we do not recognize was downgraded from retryable ServerError to
// RequestError purely because its error shape was unfamiliar, and the retry
// loop stopped retrying an outage that was clearly retryable by status.
std::optional<llm::LlmErrorCategory> CategoryForErrorType(const std::string& type) {
    if (type == "rate_limit_error") return llm::LlmErrorCategory::RateLimit;
    if (type == "authentication_error" || type == "permission_error") {
        return llm::LlmErrorCategory::AuthError;
    }
    if (type == "overloaded_error" || type == "api_error") return llm::LlmErrorCategory::ServerError;
    if (type == "invalid_request_error" || type == "not_found_error" ||
        type == "request_too_large") {
        return llm::LlmErrorCategory::RequestError;
    }
    return std::nullopt;
}

llm::LlmErrorCategory CategoryForStatus(int status) {
    if (status == common::kHttpTooManyRequests) return llm::LlmErrorCategory::RateLimit;
    if (status == common::kHttpUnauthorized || status == common::kHttpForbidden) return llm::LlmErrorCategory::AuthError;
    if (status == common::kHttpRequestTimeout) return llm::LlmErrorCategory::Transport;   // gateway timeout analogue
    if (status >= common::kHttpInternalServerError) return llm::LlmErrorCategory::ServerError;
    return llm::LlmErrorCategory::RequestError;
}

// Standard "Retry-After" only (seconds) -- unlike OpenAI, Anthropic has no
// documented millisecond-precision variant to prefer. Hostile or absurd
// values (negative, overflow, an HTTP-date) leave the hint unset rather than
// turning into a wild wait, same discipline as provider-openai's parser.
std::optional<std::chrono::milliseconds> ParseRetryAfter(
    const std::vector<net::HttpHeader>& headers) {
    const auto value = net::FindHeaderCI(headers, "Retry-After");
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
                                  const std::vector<net::HttpHeader>& headers) {
    const ParsedError parsed = ParseErrorBody(body);
    const std::optional<std::chrono::milliseconds> retry_after = ParseRetryAfter(headers);

    // A parsed-but-unrecognized type defers to the status; only a recognized
    // type overrides it (retryable 408/429/5xx stay retryable either way).
    const auto by_type =
        parsed.ok && !parsed.type.empty() ? CategoryForErrorType(parsed.type) : std::nullopt;
    const llm::LlmErrorCategory category = by_type.value_or(CategoryForStatus(status));
    const std::string message =
        parsed.ok && !parsed.message.empty() ? parsed.message : ("HTTP " + std::to_string(status));
    return llm::LlmError(category, message, status, retry_after);
}

llm::LlmError ClassifyErrorEventData(std::string_view data) {
    // The payload of an `event: error` SSE frame -- how Anthropic delivers a
    // mid-stream failure (most commonly overloaded_error). No HTTP status or
    // headers travel with it, so there is no Retry-After to parse here.
    const ParsedError parsed = ParseErrorBody(std::string(data));
    // No HTTP status travels with an SSE error frame, so an unrecognized type
    // falls back to ServerError (retryable) -- the mid-stream analogue of the
    // status-based fallback above, and the same default the no-type case
    // always used.
    const auto by_type =
        parsed.ok && !parsed.type.empty() ? CategoryForErrorType(parsed.type) : std::nullopt;
    const llm::LlmErrorCategory category =
        by_type.value_or(llm::LlmErrorCategory::ServerError);
    // The raw-frame fallback is capped: the frame is server/gateway-authored
    // and bounded only by the SSE parser's 1 MiB line cap, so an unbounded
    // embed would put a mebibyte of remote text into the exception message
    // -- and through it, into the session error the model and user see.
    constexpr std::size_t kMaxEmbeddedError = 2000;
    std::string message = parsed.ok && !parsed.message.empty() ? parsed.message : std::string(data);
    if (message.size() > kMaxEmbeddedError) {
        message.resize(kMaxEmbeddedError);
        message += " …[truncated]";
    }
    return llm::LlmError(category, message);
}

llm::LlmError ClassifyTransportFailure(const net::HttpError& error) {
    return llm::LlmError(llm::LlmErrorCategory::Transport, error.what());
}

}  // namespace minimal::provider_anthropic
