#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace minimal::net {

struct HttpHeader {
    std::string name, value;
};

// Case-insensitive header lookup -- the one piece of HttpClient logic that's
// pure enough to unit test without a real transport.
std::optional<std::string> FindHeaderCI(const std::vector<HttpHeader>& headers, std::string_view name);

// The pairwise compare FindHeaderCI is built on, exposed for callers (e.g. a
// streaming callback's own per-response header vector) that need to compare
// header names directly rather than look one up in an assembled HttpResponse.
bool HeaderNameEquals(std::string_view a, std::string_view b);

struct HttpRequest {
    std::string url;
    std::vector<HttpHeader> headers;
    std::string body;
    std::chrono::milliseconds connect_timeout{5000};
    std::chrono::milliseconds request_timeout{0};  // 0 = uncapped; caps the WHOLE transfer in every mode
    std::chrono::milliseconds idle_timeout{0};     // 0 = disabled; StreamPost() only
    // Optional CURLOPT_RESOLVE entries ("host:port:addr"). For callers that
    // have ALREADY resolved and vetted the host themselves (the tools-web SSRF
    // policy passes the addresses its check validated): the transfer connects
    // to the pinned addresses instead of letting libcurl re-resolve, so
    // check-time and connect-time resolution cannot disagree (DNS rebinding
    // TOCTOU). Empty by default -- libcurl resolves normally.
    std::vector<std::string> pinned_resolves;
};

struct HttpResponse {
    int status = 0;
    std::string body;  // full body for Post() and Get(); unused by StreamPost()
    std::vector<HttpHeader> headers;
};

enum class HttpErrorKind { ConnectFailed, TlsError, Timeout, Cancelled, Other };

// Bounds a buffered GET response (model catalogs) in bytes -- a transfer past
// it aborts with HttpError{Other} rather than buffering unboundedly. Post()
// carries a larger internal cap (see http_client.cpp) because legitimate
// completions can be large.
inline constexpr std::size_t kMaxResponseBodyBytes = 8u << 20;  // 8 MiB

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

    // GET with a buffered response body. Same timeout semantics as Post():
    // request_timeout > 0 caps the whole transfer, 0 is uncapped. The body is
    // buffered (bounded by an internal 8 MiB cap -- larger responses throw
    // HttpError{Other}). Used by model catalogs (track N).
    HttpResponse Get(const HttpRequest& request) const;

    HttpResponse StreamPost(const HttpRequest& request, const HttpHeadersReadySink& on_headers,
                            const HttpBodyChunkSink& on_chunk) const;
};

// One CURL handle, reused across transfers: the connection cache lives in the
// handle, so consecutive requests to the same host ride one TCP+TLS
// connection (keep-alive, session resumption) instead of paying a full
// handshake per call. The MCP http transport -- one serialized worker thread
// making many requests to one URL -- is exactly this shape; HttpClient's
// per-call handles are right for providers, which stream for free minutes
// apart and must stay usable from many threads.
//
// NOT thread-safe: one thread at a time, by contract. State is reset after
// EVERY transfer -- success, transport failure, or sink abort -- so a
// poisoned transfer cannot leak into the next one. Response code and
// semantics are identical to HttpClient's.
class HttpSession {
public:
    HttpSession();
    ~HttpSession();
    HttpSession(const HttpSession&) = delete;
    HttpSession& operator=(const HttpSession&) = delete;

    HttpResponse Post(const HttpRequest& request) const;
    HttpResponse StreamPost(const HttpRequest& request, const HttpHeadersReadySink& on_headers,
                             const HttpBodyChunkSink& on_chunk) const;

private:
    // The curl easy handle, type-erased: libcurl's CURL typedef differs
    // across platforms (void* on some, a struct pointer on others), and this
    // public header must stay curl-free (curl is a PRIVATE dependency;
    // plugins include llm headers). The implementation casts.
    void* handle_ = nullptr;  // never null after the constructor
};

}  // namespace minimal::net
