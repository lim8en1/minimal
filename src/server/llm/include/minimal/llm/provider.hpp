#pragma once

#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>

#include "minimal/llm/chat.hpp"
#include "minimal/llm/stream_event.hpp"

namespace minimal::llm {

struct ProviderConfig {
    std::string base_url;
    std::string api_key;
    std::string model_name;
    std::chrono::milliseconds connect_timeout{5000};
    std::chrono::milliseconds request_timeout{120000};
    std::chrono::milliseconds stream_idle_timeout{60000};
};

enum class LlmErrorCategory { Transport, RateLimit, ServerError, AuthError, RequestError, ParseError };

// One error type with a category enum rather than a hierarchy: it has to cross
// two boundaries cleanly -- the agent loop's retry decision, and translation
// into a gRPC error payload -- and a hierarchy would need RTTI dispatch at the
// first and a message-per-type at the second.
class LlmError : public std::runtime_error {
public:
    LlmError(LlmErrorCategory category, const std::string& message,
             std::optional<int> http_status = std::nullopt,
             std::optional<std::chrono::milliseconds> retry_after = std::nullopt)
        : std::runtime_error(message),
          category_(category),
          http_status_(http_status),
          retry_after_(retry_after) {}

    LlmErrorCategory category() const { return category_; }
    std::optional<int> http_status() const { return http_status_; }
    std::optional<std::chrono::milliseconds> retry_after() const { return retry_after_; }

    bool is_retryable() const {
        switch (category_) {
            case LlmErrorCategory::Transport:
            case LlmErrorCategory::RateLimit:
            case LlmErrorCategory::ServerError:
                return true;
            case LlmErrorCategory::AuthError:
            case LlmErrorCategory::RequestError:
            case LlmErrorCategory::ParseError:
                return false;
        }
        return false;
    }

private:
    LlmErrorCategory category_;
    std::optional<int> http_status_;
    std::optional<std::chrono::milliseconds> retry_after_;
};

// Deliberately two methods. An earlier QueryExtension/ProviderExtension
// capability-query layer was deleted once its only consumer (llama.cpp KV-cache
// slots) moved to a plugin.
//
// Thread-safety contract, load-bearing because instances are shared across
// concurrent sessions by the provider instance cache: implementations must hold
// NO mutable state across calls -- one HTTP handle per call, stream-reassembly
// state local to the call.
class LlmProvider {
public:
    virtual ~LlmProvider() = default;

    virtual ChatResponse Complete(const ChatRequest& request) = 0;

    // Returns the assembled response, not void: the caller needs the final
    // message to append to history, and the provider already maintains exactly
    // that state in order to reassemble streamed tool-call fragments. Returning
    // it stops every caller re-deriving it from the event stream.
    //
    // Retries are NOT performed here -- this throws a classified LlmError and
    // the agent loop owns backoff and attempt limits, since only it knows
    // whether a retry is still wanted.
    virtual ChatResponse StreamComplete(const ChatRequest& request, const StreamEventSink& sink) = 0;
};

}  // namespace minimal::llm
