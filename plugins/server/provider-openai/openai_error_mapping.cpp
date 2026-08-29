#include "openai_error_mapping.hpp"

#include <chrono>
#include <optional>

#include <nlohmann/json.hpp>

namespace minimal::provider_openai {

namespace {

std::string ExtractMessage(const std::string& body, int status) {
    try {
        auto json = nlohmann::json::parse(body);
        if (json.contains("error") && json.at("error").contains("message")) {
            return json.at("error").at("message").get<std::string>();
        }
    } catch (const nlohmann::json::exception&) {
        // Not JSON, or not the expected shape -- fall through to a generic message.
    }
    return "HTTP " + std::to_string(status);
}

}  // namespace

llm::LlmError ClassifyHttpFailure(int status, const std::string& body, const std::vector<llm::HttpHeader>& headers) {
    std::string message = ExtractMessage(body, status);

    if (status == 429) {
        std::optional<std::chrono::milliseconds> retry_after;
        if (auto value = llm::FindHeaderCI(headers, "Retry-After")) {
            try {
                retry_after = std::chrono::milliseconds(std::stoll(*value) * 1000);
            } catch (const std::exception&) {
                // Non-numeric Retry-After (e.g. an HTTP-date) -- leave unset.
            }
        }
        return llm::LlmError(llm::LlmErrorCategory::RateLimit, message, status, retry_after);
    }
    if (status == 401 || status == 403) {
        return llm::LlmError(llm::LlmErrorCategory::AuthError, message, status);
    }
    if (status >= 500) {
        return llm::LlmError(llm::LlmErrorCategory::ServerError, message, status);
    }
    return llm::LlmError(llm::LlmErrorCategory::RequestError, message, status);
}

llm::LlmError ClassifyTransportFailure(const llm::HttpError& error) {
    return llm::LlmError(llm::LlmErrorCategory::Transport, error.what());
}

}  // namespace minimal::provider_openai
