#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace minimal::llm {

struct HttpHeader {
    std::string name, value;
};

// Case-insensitive header lookup -- the one piece of HttpClient logic that's
// pure enough to unit test without a real transport.
std::optional<std::string> FindHeaderCI(const std::vector<HttpHeader>& headers, std::string_view name);

struct HttpRequest {
    std::string url;
    std::vector<HttpHeader> headers;
    std::string body;
    std::chrono::milliseconds connect_timeout{5000};
    std::chrono::milliseconds request_timeout{0};  // 0 = uncapped; Post() only
    std::chrono::milliseconds idle_timeout{0};     // 0 = disabled; StreamPost() only
};

struct HttpResponse {
    int status = 0;
    std::string body;  // full body for Post(); unused by StreamPost()
    std::vector<HttpHeader> headers;
};

enum class HttpErrorKind { ConnectFailed, TlsError, Timeout, Cancelled, Other };

// Transport-level failure only -- a non-2xx status is NOT an error here. It
// comes back via HttpResponse::status so the caller can classify it against
// whatever error-category scheme it needs, using the body and headers too.
class HttpError : public std::runtime_error {
public:
    HttpError(HttpErrorKind kind, const std::string& message) : std::runtime_error(message), kind_(kind) {}

    HttpErrorKind kind() const { return kind_; }

private:
    HttpErrorKind kind_;
};

// Return false to abort the transfer -- HttpClient throws HttpError{Cancelled}
// rather than a generic transport error. Mirrors StreamEventSink one layer up:
// the abort propagates through libcurl's C write callback (a short byte count,
// yielding CURLE_WRITE_ERROR), so this must never throw across that boundary.
using HttpBodyChunkSink = std::function<bool(std::string_view chunk)>;

// Fires once, when status and headers are fully known but before the first
// on_chunk() call -- libcurl always completes headers before delivering body
// bytes for a given response.
using HttpHeadersReadySink = std::function<void(int status, const std::vector<HttpHeader>& headers)>;

// Stateless: every call opens and tears down its own CURL handle, so a shared
// HttpClient value has no data race across concurrent calls. That's what lets
// a caller hold one as a plain member without violating LlmProvider's
// "no mutable state across calls" contract.
class HttpClient {
public:
    HttpResponse Post(const HttpRequest& request) const;

    HttpResponse StreamPost(const HttpRequest& request, const HttpHeadersReadySink& on_headers,
                             const HttpBodyChunkSink& on_chunk) const;
};

}  // namespace minimal::llm
