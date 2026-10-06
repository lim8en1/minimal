#pragma once

#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>

#include "minimal/common/secret_handle.hpp"
#include "minimal/llm/chat.hpp"
#include "minimal/llm/stream_event.hpp"
#include "minimal/common/policy.hpp"
#include "minimal/llm/policy.hpp"

namespace minimal::llm {

struct ProviderConfig {
    std::string base_url;
    // Read per request (Get), so a secret stored with /secret set reaches the
    // next request. Unconfigured for a keyless server.
    common::SecretHandle api_key;
    std::string model_name;
    // Extra HTTP headers from providers.json ("headers": {"X-Thing": "value"},
    // or the opencode-compat options.headers). The bundled chat providers
    // append these after their own required headers; a header duplicating one
    // the provider already sets (Content-Type, the auth header) is the
    // operator's to avoid.
    std::vector<std::pair<std::string, std::string>> headers;
    // The JSON body key the request's output cap (ChatRequest::max_tokens) is
    // serialized under. Empty means "max_tokens" -- the historical
    // chat-completions field every OpenAI-compatible local backend accepts,
    // and what newer OpenAI models REJECT in favor of "max_completion_tokens".
    // providers.json's "max_tokens_field" sets it per entry; the OpenAI
    // provider consumes it, other backends ignore it.
    std::string max_tokens_field;
    std::chrono::milliseconds connect_timeout = common::kDefaultConnectTimeout;
    std::chrono::milliseconds request_timeout = kDefaultRequestTimeout;
    std::chrono::milliseconds stream_idle_timeout = kDefaultStreamIdleTimeout;
    // Longest wait for the FIRST response byte once the request is fully sent.
    // Longer than the idle timeout because a local backend prefilling a
    // 300k-1M token prompt legitimately says nothing for minutes; a cancel
    // still lands within about a second (ChatRequest::should_abort). 0 = use
    // stream_idle_timeout.
    std::chrono::milliseconds first_byte_timeout = kDefaultFirstByteTimeout;
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

// The connection's key for one request, or nullopt for a keyless connection.
// A reference that does not resolve right now (a secret not stored yet, a
// host binding refusing this url) is an AuthError: retrying cannot fix it.
inline std::optional<std::string> RequestApiKey(const ProviderConfig& config) {
    if (!config.api_key.configured()) return std::nullopt;
    try {
        return config.api_key.Get().value;
    } catch (const std::exception& error) {
        throw LlmError(LlmErrorCategory::AuthError, std::string("api key: ") + error.what());
    }
}

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

    // True when the backend binds a replayed thinking block to the exact
    // conversation prefix that produced it and rejects the replay once that
    // prefix changed (newer Claude models). The agent loop then drops the
    // thinking of the messages a compaction keeps verbatim. Backends that
    // never replay thinking (chat completions drops it on the wire) keep the
    // default.
    virtual bool BindsThinkingToPrefix() const { return false; }
};

}  // namespace minimal::llm
