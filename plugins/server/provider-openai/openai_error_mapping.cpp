#include "openai_error_mapping.hpp"

#include <cctype>
#include <chrono>
#include <optional>

#include <nlohmann/json.hpp>
#include "minimal/common/units.hpp"
#include "minimal/common/clock.hpp"

namespace minimal::provider_openai {

namespace {

// Only the head of an error body is ever parsed for a message. The body can
// be as large as the HTTP client's 64 MiB cap; handing all of it to nlohmann
// to extract one string is a self-inflicted stall on a hostile (or merely
// absurd) error body. 64 KiB is the same bound the streaming path applies.
constexpr std::size_t kMaxErrorBodyParse = 64 * common::kKiB;
// And no extracted/embedded message grows beyond this either.
constexpr std::size_t kMaxErrorMessage = 2000;

std::string_view HeadOf(const std::string& body) {
    std::string_view slice(body);
    if (slice.size() > kMaxErrorBodyParse) slice = slice.substr(0, kMaxErrorBodyParse);
    return slice;
}

std::string CapMessage(std::string message) {
    if (message.size() > kMaxErrorMessage) {
        message.resize(kMaxErrorMessage);
        message += " …[truncated]";
    }
    return message;
}

std::string ExtractMessage(const std::string& body, int status) {
    try {
        auto json = nlohmann::json::parse(HeadOf(body));
        if (json.contains("error") && json.at("error").contains("message")) {
            return CapMessage(json.at("error").at("message").get<std::string>());
        }
    } catch (const nlohmann::json::exception&) {
        // Not JSON, or not the expected shape (including a slice truncated
        // mid-document) -- fall through to a generic message.
    }
    return "HTTP " + std::to_string(status);
}

// Retry-After in EITHER wire form -- seconds ("Retry-After: 30") or
// milliseconds ("retry-after-ms: 2500", what OpenAI's own 429s carry) -- and
// for ANY status that carries one, not just 429: OpenRouter and vLLM attach
// it to 503/529 overloads routinely. Hostile or absurd values (negative,
// overflow, an HTTP-date in the ms field) leave the hint unset rather than
// turning into a wild wait.
std::optional<std::chrono::milliseconds> ParseRetryAfter(const std::vector<net::HttpHeader>& headers) {
    std::optional<std::string> value = net::FindHeaderCI(headers, "retry-after-ms");
    bool is_ms = true;
    if (!value) {
        value = net::FindHeaderCI(headers, "Retry-After");
        is_ms = false;
    }
    if (!value) return std::nullopt;
    try {
        std::size_t consumed = 0;
        const long long parsed = std::stoll(*value, &consumed);
        if (parsed <= 0 || consumed != value->size()) return std::nullopt;
        // The bound is checked on `parsed` itself, BEFORE the *1000
        // multiply below, not on the resulting duration: a value
        // std::stoll can parse but that is still absurd (e.g.
        // "9999999999999999" seconds) overflowed the signed 64-bit
        // multiply -- UB -- before a post-multiply `duration > minutes(10)`
        // check ever ran. 10 minutes is above any real rate-limit window.
        constexpr long long kMaxMs = std::chrono::milliseconds(std::chrono::minutes(10)).count();
        if (is_ms) {
            if (parsed > kMaxMs) return std::nullopt;
            return std::chrono::milliseconds(parsed);
        }
        if (parsed > common::MillisToSeconds(kMaxMs)) return std::nullopt;
        return std::chrono::seconds(parsed);
    } catch (const std::exception&) {
        return std::nullopt;  // non-numeric (e.g. an HTTP-date) -- leave unset
    }
}

}  // namespace

llm::LlmError ClassifyHttpFailure(int status, const std::string& body, const std::vector<net::HttpHeader>& headers) {
    std::string message = ExtractMessage(body, status);
    std::optional<std::chrono::milliseconds> retry_after = ParseRetryAfter(headers);

    if (status == 429) {
        return llm::LlmError(llm::LlmErrorCategory::RateLimit, message, status, retry_after);
    }
    if (status == 401 || status == 403) {
        return llm::LlmError(llm::LlmErrorCategory::AuthError, message, status);
    }
    if (status == 408) {
        // Request Timeout is the HTTP analogue of the transport timeouts the
        // loop retries -- Azure-fronted gateways emit it routinely -- and used
        // to fall to RequestError, non-retryable, failing a whole turn on one
        // transient gateway timeout.
        return llm::LlmError(llm::LlmErrorCategory::Transport, message, status, retry_after);
    }
    if (status >= 500) {
        return llm::LlmError(llm::LlmErrorCategory::ServerError, message, status, retry_after);
    }
    return llm::LlmError(llm::LlmErrorCategory::RequestError, message, status);
}

llm::LlmError ClassifyErrorEventData(std::string_view data) {
    // The payload of an `event: error` SSE frame. Whatever it says, the stream
    // is over and the server said why -- ServerError (retryable) unless the
    // body names an auth failure, so a gateway blip retries and a bad key
    // does not hammer it.
    std::string message(data);
    llm::LlmErrorCategory category = llm::LlmErrorCategory::ServerError;
    try {
        auto json = nlohmann::json::parse(HeadOf(std::string(data)));
        if (json.contains("error") && json.at("error").contains("message") &&
            json.at("error").at("message").is_string()) {
            message = json.at("error").at("message").get<std::string>();
        }
        const std::string lowered = [&] {
            std::string out = message;
            for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return out;
        }();
        if (lowered.find("unauthorized") != std::string::npos ||
            lowered.find("invalid api key") != std::string::npos ||
            lowered.find("authentication") != std::string::npos) {
            category = llm::LlmErrorCategory::AuthError;
        }
    } catch (const nlohmann::json::exception&) {
        // Not JSON: the raw data is the best message there is.
    }
    return llm::LlmError(category, CapMessage(std::move(message)));
}

llm::LlmError ClassifyTransportFailure(const net::HttpError& error) {
    return llm::LlmError(llm::LlmErrorCategory::Transport, error.what());
}

}  // namespace minimal::provider_openai
