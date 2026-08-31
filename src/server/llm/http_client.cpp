#include "minimal/llm/http_client.hpp"

#include <cctype>
#include <chrono>

#include <curl/curl.h>

namespace minimal::llm {

namespace {

bool IEquals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

void EnsureGlobalInit() {
    struct CurlGlobalInit {
        CurlGlobalInit() { curl_global_init(CURL_GLOBAL_DEFAULT); }
        ~CurlGlobalInit() { curl_global_cleanup(); }
    };
    static CurlGlobalInit init;
}

struct CurlHandle {
    CURL* handle = curl_easy_init();
    CurlHandle() = default;
    ~CurlHandle() {
        if (handle) curl_easy_cleanup(handle);
    }
    CurlHandle(const CurlHandle&) = delete;
    CurlHandle& operator=(const CurlHandle&) = delete;
};

struct SListGuard {
    curl_slist* list = nullptr;
    ~SListGuard() {
        if (list) curl_slist_free_all(list);
    }
};

// Skips the "HTTP/x.y NNN ..." status line and the blank line terminating the
// header block; parses everything else as "Name: value".
bool ParseHeaderLine(std::string_view line, HttpHeader& out) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.remove_suffix(1);
    if (line.empty()) return false;
    auto colon = line.find(':');
    if (colon == std::string_view::npos) return false;
    std::string_view value = line.substr(colon + 1);
    while (!value.empty() && value.front() == ' ') value.remove_prefix(1);
    out.name = std::string(line.substr(0, colon));
    out.value = std::string(value);
    return true;
}

enum class Mode { Blocking, Streaming };

struct TransferState {
    Mode mode = Mode::Blocking;
    CURL* handle = nullptr;
    HttpResponse response;
    const HttpHeadersReadySink* on_headers = nullptr;
    const HttpBodyChunkSink* on_chunk = nullptr;
    bool headers_ready_fired = false;
    bool cancelled_by_sink = false;

    // Idle-timeout bookkeeping -- StreamPost() only.
    std::chrono::milliseconds idle_timeout{0};
    std::chrono::steady_clock::time_point last_progress = std::chrono::steady_clock::now();
    curl_off_t last_dlnow = 0;
};

// Called lazily on the first body byte rather than by detecting the blank
// header-terminator line: libcurl always finishes delivering headers before
// the first write callback for the same response, so this is equivalent and
// avoids relying on exact blank-line framing.
void MaybeFireHeadersReady(TransferState& state) {
    if (state.headers_ready_fired || state.mode != Mode::Streaming || !state.on_headers) return;
    long status = 0;
    curl_easy_getinfo(state.handle, CURLINFO_RESPONSE_CODE, &status);
    state.response.status = static_cast<int>(status);
    (*state.on_headers)(state.response.status, state.response.headers);
    state.headers_ready_fired = true;
}

std::size_t HeaderCallback(char* buffer, std::size_t size, std::size_t nitems, void* userdata) {
    auto& state = *static_cast<TransferState*>(userdata);
    std::size_t total = size * nitems;
    HttpHeader header;
    if (ParseHeaderLine(std::string_view(buffer, total), header)) {
        state.response.headers.push_back(std::move(header));
    }
    return total;
}

std::size_t WriteCallback(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    auto& state = *static_cast<TransferState*>(userdata);
    std::size_t total = size * nmemb;
    if (state.mode == Mode::Blocking) {
        state.response.body.append(ptr, total);
        return total;
    }
    MaybeFireHeadersReady(state);
    if (state.on_chunk && !(*state.on_chunk)(std::string_view(ptr, total))) {
        state.cancelled_by_sink = true;
        return 0;  // short count -> CURLE_WRITE_ERROR
    }
    return total;
}

int XferInfoCallback(void* clientp, curl_off_t /*dltotal*/, curl_off_t dlnow, curl_off_t /*ultotal*/,
                      curl_off_t /*ulnow*/) {
    auto& state = *static_cast<TransferState*>(clientp);
    auto now = std::chrono::steady_clock::now();
    if (dlnow != state.last_dlnow) {
        state.last_dlnow = dlnow;
        state.last_progress = now;
        return 0;
    }
    if (now - state.last_progress > state.idle_timeout) return 1;  // nonzero -> abort
    return 0;
}

[[noreturn]] void ThrowForCurlCode(CURLcode code, const TransferState& state) {
    if (code == CURLE_WRITE_ERROR && state.cancelled_by_sink) {
        throw HttpError(HttpErrorKind::Cancelled, "aborted by sink");
    }
    switch (code) {
        case CURLE_ABORTED_BY_CALLBACK:
            throw HttpError(HttpErrorKind::Timeout, "stream idle timeout exceeded");
        case CURLE_OPERATION_TIMEDOUT:
            throw HttpError(HttpErrorKind::Timeout, curl_easy_strerror(code));
        case CURLE_COULDNT_CONNECT:
        case CURLE_COULDNT_RESOLVE_HOST:
        case CURLE_COULDNT_RESOLVE_PROXY:
            throw HttpError(HttpErrorKind::ConnectFailed, curl_easy_strerror(code));
        case CURLE_SSL_CONNECT_ERROR:
        case CURLE_PEER_FAILED_VERIFICATION:  // == CURLE_SSL_CACERT
        case CURLE_SSL_CACERT_BADFILE:
        case CURLE_SSL_CERTPROBLEM:
        case CURLE_SSL_CIPHER:
        case CURLE_SSL_ISSUER_ERROR:
            throw HttpError(HttpErrorKind::TlsError, curl_easy_strerror(code));
        default:
            throw HttpError(HttpErrorKind::Other, curl_easy_strerror(code));
    }
}

// Configures `handle` for one POST transfer, performs it, and returns the
// response. The caller owns the handle's lifetime; everything set here is
// per-transfer, undone by a curl_easy_reset between transfers on a reused
// handle.
HttpResponse ConfigureAndPerform(CURL* handle, const HttpRequest& request, Mode mode,
                                 const HttpHeadersReadySink* on_headers,
                                 const HttpBodyChunkSink* on_chunk) {
    TransferState state;
    state.mode = mode;
    state.handle = handle;
    state.on_headers = on_headers;
    state.on_chunk = on_chunk;
    state.idle_timeout = request.idle_timeout;
    state.last_progress = std::chrono::steady_clock::now();

    curl_easy_setopt(handle, CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(handle, CURLOPT_POST, 1L);
    curl_easy_setopt(handle, CURLOPT_POSTFIELDS, request.body.data());
    curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(request.connect_timeout.count()));

    // The cap is total, not per-phase, in BOTH modes: a streaming server that
    // accepts the connection and then goes silent must degrade to a bounded
    // error, never wedge the calling thread. Streaming callers that need an
    // unbounded transfer (LLM providers) leave this 0.
    if (request.request_timeout.count() > 0) {
        curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, static_cast<long>(request.request_timeout.count()));
    }

    SListGuard headers_guard;
    for (const auto& header : request.headers) {
        headers_guard.list = curl_slist_append(headers_guard.list, (header.name + ": " + header.value).c_str());
    }
    if (headers_guard.list) curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers_guard.list);

    curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, &HeaderCallback);
    curl_easy_setopt(handle, CURLOPT_HEADERDATA, &state);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, &WriteCallback);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &state);

    if (mode == Mode::Streaming && request.idle_timeout.count() > 0) {
        curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, &XferInfoCallback);
        curl_easy_setopt(handle, CURLOPT_XFERINFODATA, &state);
    }

    CURLcode code = curl_easy_perform(handle);
    if (code != CURLE_OK) ThrowForCurlCode(code, state);

    long status = 0;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
    state.response.status = static_cast<int>(status);
    return std::move(state.response);
}

HttpResponse PerformRequest(const HttpRequest& request, Mode mode, const HttpHeadersReadySink* on_headers,
                            const HttpBodyChunkSink* on_chunk) {
    EnsureGlobalInit();
    CurlHandle curl;
    if (!curl.handle) throw HttpError(HttpErrorKind::Other, "curl_easy_init failed");
    return ConfigureAndPerform(curl.handle, request, mode, on_headers, on_chunk);
}

}  // namespace

std::optional<std::string> FindHeaderCI(const std::vector<HttpHeader>& headers, std::string_view name) {
    for (const auto& header : headers) {
        if (IEquals(header.name, name)) return header.value;
    }
    return std::nullopt;
}

HttpResponse HttpClient::Post(const HttpRequest& request) const {
    return PerformRequest(request, Mode::Blocking, nullptr, nullptr);
}

HttpResponse HttpClient::StreamPost(const HttpRequest& request, const HttpHeadersReadySink& on_headers,
                                     const HttpBodyChunkSink& on_chunk) const {
    return PerformRequest(request, Mode::Streaming, &on_headers, &on_chunk);
}

// --- HttpSession -------------------------------------------------------------

namespace {

CURL* AsCurl(void* handle) { return static_cast<CURL*>(handle); }

}  // namespace

HttpSession::HttpSession() {
    EnsureGlobalInit();
    handle_ = curl_easy_init();
    if (handle_ == nullptr) throw HttpError(HttpErrorKind::Other, "curl_easy_init failed");
}

HttpSession::~HttpSession() {
    if (handle_ != nullptr) curl_easy_cleanup(AsCurl(handle_));
}

// The reset must run after EVERY transfer -- success, transport failure, or
// sink abort: options are per-transfer (a request_timeout set once would leak
// into an unbounded later transfer), while the connection cache survives the
// reset. That asymmetry is the whole reason this class works.
namespace {

// The reset must run after EVERY transfer -- success, transport failure, or
// sink abort: options are per-transfer (a request_timeout set once would leak
// into an unbounded later transfer), while the connection cache survives the
// reset. That asymmetry is the whole reason HttpSession works.
struct ResetOnScopeExit {
    CURL* handle;
    ~ResetOnScopeExit() { curl_easy_reset(handle); }
};

}  // namespace

HttpResponse HttpSession::Post(const HttpRequest& request) const {
    CURL* curl = AsCurl(handle_);
    ResetOnScopeExit reset{curl};
    return ConfigureAndPerform(curl, request, Mode::Blocking, nullptr, nullptr);
}

HttpResponse HttpSession::StreamPost(const HttpRequest& request, const HttpHeadersReadySink& on_headers,
                                     const HttpBodyChunkSink& on_chunk) const {
    CURL* curl = AsCurl(handle_);
    ResetOnScopeExit reset{curl};
    return ConfigureAndPerform(curl, request, Mode::Streaming, &on_headers, &on_chunk);
}

}  // namespace minimal::llm
