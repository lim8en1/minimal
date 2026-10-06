#include "minimal/mcp/mcp_connection.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include "minimal/common/logger.hpp"
#include "minimal/sidecar/process.hpp"
#include "minimal/net/http_client.hpp"
#include "minimal/net/sse_parser.hpp"
#include "minimal/common/units.hpp"
#include "minimal/common/http_status.hpp"
#include "policy.hpp"
#include "minimal/common/policy.hpp"
#include "minimal/sidecar/policy.hpp"

namespace minimal::mcp {

namespace {

constexpr const char* kProtocolVersion = "2025-03-26";
constexpr const char* kClientName = "minimal";
constexpr const char* kClientVersion = "1.0.0b";

// Completion slot for one call. Shared between caller and worker: the caller
// may give up on deadline or cancellation while the job still runs, so the
// slot must outlive the caller's interest. Waiters do NOT wait on the slot --
// they wait on the call's cancellation token (see WaitSlot), whose cv the
// completion path notifies too.
template <typename T>
struct Slot {
    std::mutex mu;  // guards value/error
    std::optional<T> value;
    std::string error;
    std::atomic<bool> done{false};  // set LAST, release: publish after value/error

    void Complete(std::optional<T> v, std::string e) {
        {
            std::lock_guard<std::mutex> lock(mu);
            value = std::move(v);
            error = std::move(e);
        }
        done.store(true, std::memory_order_release);
    }
};

// A null token means "no cancellation path": the wait still needs a cv to be
// woken by the completion notify, so it shares this process-wide dummy. It is
// never cancelled, and a stray wake from another null-token completion is
// harmless -- waiters re-check their own slot.
tools::CancelToken& TokenFor(const tools::CancelTokenPtr& cancel) {
    static tools::CancelToken never_cancelled;
    return cancel != nullptr ? *cancel : never_cancelled;
}

// How a wait ended. Typed rather than a sentinel string in `error`: the
// caller-cancelled case changes what the manager does to the connection
// (leave it alive -- the server did nothing wrong), and detecting that by
// comparing the human-facing message against the literal "cancelled" broke
// silently under any rename.
enum class WaitResult { kCompleted, kCallerCancelled, kTimedOut, kSlotError };

// Waits until the slot completes, the token is cancelled, or the deadline
// passes. Zero polling: the completion path notifies the token's cv, so the
// waiter wakes on exactly the events that can change its outcome. kCompleted
// when the slot completed; otherwise `error` is set -- the underlying job
// keeps running in the background either way.
//
// Lock order: the token mutex is held across the predicate loop and the wait,
// then RELEASED before slot.mu is taken -- the worker's completion path takes
// them in the opposite order (slot.mu, then token.mu in Notify), and a
// waiter that held both would close a cycle.
template <typename T>
WaitResult WaitSlot(Slot<T>& slot, const tools::CancelTokenPtr& cancel,
                    std::chrono::steady_clock::time_point deadline, std::string& error) {
    tools::CancelToken& token = TokenFor(cancel);
    {
        std::unique_lock<std::mutex> lock(token.Mu());
        while (!slot.done.load(std::memory_order_acquire)) {
            if (token.IsCancelled()) {
                error = "cancelled";
                return WaitResult::kCallerCancelled;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                error = "request timed out";
                return WaitResult::kTimedOut;
            }
            token.Cv().wait_until(lock, deadline);
        }
    }
    std::lock_guard<std::mutex> slot_lock(slot.mu);
    if (slot.error.empty()) return WaitResult::kCompleted;
    error = slot.error;
    return WaitResult::kSlotError;
}

// We send integer ids; a conforming server echoes the id verbatim. Lenient
// anyway: an integer, or a string that parses to the same integer (some
// server SDKs stringify ids). Server-originated requests (notifications and
// their own request ids) must not match, and non-numeric strings cannot.
bool FrameIdMatches(const nlohmann::json& message, int64_t id) {
    if (!message.is_object() || !message.contains("id")) return false;
    // A JSON-RPC REQUEST also carries an "id" (that's what distinguishes it
    // from a notification) -- only "result"/"error" mark this frame as a
    // RESPONSE. Our own id counter and a server's own outbound request-id
    // counter both commonly start at 1, so without this check a
    // server-initiated request (e.g. sampling/createMessage) whose id
    // happens to collide with ours would be mistaken for our answer,
    // completing the slot with whatever "result" a request object doesn't
    // even have (an empty/garbage value) while the real response is ignored.
    if (!message.contains("result") && !message.contains("error")) return false;
    const auto& wire = message.at("id");
    if (wire.is_number_integer()) return wire.get<int64_t>() == id;
    if (wire.is_string()) {
        const std::string& text = wire.get_ref<const std::string&>();
        if (text.empty()) return false;
        std::size_t consumed = 0;
        long long parsed = 0;
        try {
            parsed = std::stoll(text, &consumed);
        } catch (const std::exception&) {
            return false;  // non-numeric string: never our echo
        }
        return consumed == text.size() && static_cast<int64_t>(parsed) == id;
    }
    return false;
}

}  // namespace

// --- transport -------------------------------------------------------------

struct McpConnection::Transport {
    virtual ~Transport() = default;
    // All methods BLOCK the worker thread until they complete their slot.
    virtual void Initialize(Slot<bool>& slot) = 0;
    virtual void Ping(Slot<bool>& slot) = 0;
    virtual void ListTools(Slot<std::vector<ToolListing>>& slot) = 0;
    virtual void CallTool(const std::string& tool_name, const nlohmann::json& arguments,
                          Slot<nlohmann::json>& slot) = 0;
    // The initialize result's capabilities/instructions, for channel
    // detection. Not worker-thread work: plain reads of state initialize
    // already wrote, ordered by Open's slot-wait.
    virtual std::string ServerCapabilitiesJson() const { return "{}"; }
    virtual std::string Instructions() const { return {}; }
    // Teardown support, called from the DESTROYING thread while the worker may
    // be inside a blocking call: ask the stdio child to exit, which ends that
    // call. A no-op for transports without a process. Returns the child, whose
    // ownership passes to the caller: it ends the child's life (TerminateAfter),
    // on whatever thread suits it -- the child outlives this transport.
    virtual std::shared_ptr<sidecar::Child> Abort() { return nullptr; }
};

namespace {

// Turns one JSON-RPC response object into a result or an error. A JSON-RPC
// error is reported as an error string, never as a result. Its text is
// server-authored and flows verbatim into logs and the model-visible tool
// result, so it is capped here, at the source.
bool ReadResponse(const nlohmann::json& message, nlohmann::json* result, std::string* error) {
    if (message.contains("error")) {
        const auto& error_obj = message.at("error");
        // Checked, not value(): a non-string "message" degrades to "unknown"
        // rather than throwing on the worker thread.
        std::string server_message =
            error_obj.is_object() && error_obj.contains("message") &&
                    error_obj.at("message").is_string()
                ? error_obj.at("message").get<std::string>()
                : "unknown";
        if (server_message.size() > common::kMaxQuotedErrorBytes) {
            server_message.resize(common::kMaxQuotedErrorBytes);
            server_message += " …[truncated]";
        }
        *error = "MCP server error: " + server_message;
        return false;
    }
    *result = message.contains("result") ? message.at("result") : nlohmann::json::object();
    return true;
}

// The protocol shared by every transport: ping, paged tools/list and
// tools/call are the same JSON-RPC exchanges whether a request travels over
// HTTP or a child's stdio. A transport supplies only the round trip.
class JsonRpcTransport : public McpConnection::Transport {
public:
    explicit JsonRpcTransport(std::int64_t max_list_pages) : max_list_pages_(max_list_pages) {}

    void Ping(Slot<bool>& slot) override {
        nlohmann::json result;
        std::string error;
        slot.Complete(std::optional<bool>(Rpc("ping", nlohmann::json::object(), &result, &error)),
                      error);
    }

    void ListTools(Slot<std::vector<McpConnection::ToolListing>>& slot) override {
        std::vector<McpConnection::ToolListing> listings;
        std::string cursor;
        std::string error;
        // tools/list pages with a cursor; most servers never paginate, but a
        // truncated listing would silently hide tools from the model, so the
        // loop is real. Capped: a server that never stops returning
        // nextCursor would otherwise occupy this worker thread forever (each
        // page is individually within the request timeout).
        for (std::int64_t page = 0;; ++page) {
            if (page >= max_list_pages_) {
                slot.Complete(std::nullopt,
                              "MCP tools/list exceeded " + std::to_string(max_list_pages_) +
                                  " pages (the mcp plugin's max_list_pages); refusing further "
                                  "cursors from a possibly broken or malicious server");
                return;
            }
            nlohmann::json params = nlohmann::json::object();
            if (!cursor.empty()) params["cursor"] = cursor;
            nlohmann::json result;
            if (!Rpc("tools/list", params, &result, &error)) {
                slot.Complete(std::nullopt, error);
                return;
            }
            if (!result.contains("tools") || !result.at("tools").is_array()) {
                slot.Complete(std::nullopt, "MCP tools/list response has no tools array");
                return;
            }
            for (const auto& tool : result.at("tools")) {
                // Checked, not value(): a hostile server answering
                // {"name": 42} fails the LISTING with a visible error rather
                // than throwing on the worker thread.
                if (!tool.is_object() || !tool.contains("name") || !tool.at("name").is_string()) {
                    slot.Complete(std::nullopt, "MCP tools/list entry has no string 'name'");
                    return;
                }
                McpConnection::ToolListing listing;
                listing.name = tool.at("name").get<std::string>();
                listing.description =
                    tool.contains("description") && tool.at("description").is_string()
                        ? tool.at("description").get<std::string>()
                        : std::string();
                listing.input_schema = tool.contains("inputSchema") && tool.at("inputSchema").is_object()
                                           ? tool.at("inputSchema")
                                           : nlohmann::json::object();
                listings.push_back(std::move(listing));
            }
            cursor = result.contains("nextCursor") && result.at("nextCursor").is_string()
                         ? result.at("nextCursor").get<std::string>()
                         : std::string();
            if (cursor.empty()) break;
        }
        slot.Complete(std::move(listings), {});
    }

    void CallTool(const std::string& tool_name, const nlohmann::json& arguments,
                  Slot<nlohmann::json>& slot) override {
        nlohmann::json result;
        std::string error;
        if (!Rpc("tools/call", {{"name", tool_name}, {"arguments", arguments}}, &result, &error)) {
            slot.Complete(std::nullopt, error);
            return;
        }
        if (!result.is_object()) {
            slot.Complete(std::nullopt, "MCP server returned an unparseable tool result");
            return;
        }
        slot.Complete(std::move(result), {});
    }

protected:
    // One request/response round trip. False with `error` set on a transport
    // failure or a JSON-RPC error response.
    virtual bool Rpc(const std::string& method, const nlohmann::json& params,
                     nlohmann::json* result, std::string* error) = 0;

    nlohmann::json InitializeParams(const std::string& client_name,
                                    const std::string& client_version) const {
        return {{"protocolVersion", kProtocolVersion},
                {"capabilities", nlohmann::json::object()},
                {"clientInfo", {{"name", client_name}, {"version", client_version}}}};
    }

private:
    std::int64_t max_list_pages_;
};

// --- stdio transport: newline-delimited JSON-RPC over a child's stdio ------

// An unterminated line can only be a misbehaving or hostile child: a
// conforming message is newline-framed. Past this, the connection is dropped
// rather than growing the buffer without bound (kMaxStdioLineBytes). The
// reader polls in common::kCancelPollSlice slices, which bound how long
// ~StdioTransport waits for its join.

class StdioTransport final : public JsonRpcTransport {
public:
    StdioTransport(const McpEndpoint& endpoint, std::string client_name,
                   std::string client_version, NotificationSink sink,
                   std::chrono::milliseconds request_timeout, std::int64_t max_list_pages)
        : JsonRpcTransport(max_list_pages),
          argv_(endpoint.argv),
          env_(endpoint.env.begin(), endpoint.env.end()),
          client_name_(std::move(client_name)),
          client_version_(std::move(client_version)),
          sink_(std::move(sink)),
          request_timeout_(request_timeout) {}

    ~StdioTransport() override {
        stopping_.store(true, std::memory_order_release);
        if (reader_.joinable()) reader_.join();
        // A child still held here was never aborted -- the connection failed
        // before teardown could take it. It must not outlive us (Child's
        // destructor kills and reaps the group).
        std::lock_guard<std::mutex> lock(child_mu_);
        child_.reset();
    }

    void Initialize(Slot<bool>& slot) override {
        std::string error;
        if (!Start(error)) {
            slot.Complete(std::nullopt, "MCP initialize failed: could not start the server: " + error);
            return;
        }
        nlohmann::json result;
        if (!Rpc("initialize", InitializeParams(client_name_, client_version_), &result, &error)) {
            slot.Complete(std::nullopt,
                          "MCP initialize failed: server rejected the handshake or died "
                          "during startup: " +
                              error);
            return;
        }
        if (result.contains("capabilities") && result.at("capabilities").is_object()) {
            capabilities_ = result.at("capabilities").dump();
        }
        // Missing and non-string both degrade to empty.
        if (result.contains("instructions") && result.at("instructions").is_string()) {
            instructions_ = result.at("instructions").get<std::string>();
        }
        if (!WriteMessage({{"jsonrpc", "2.0"},
                           {"method", "notifications/initialized"},
                           {"params", nlohmann::json::object()}},
                          error)) {
            slot.Complete(std::nullopt, "MCP initialize failed: " + error);
            return;
        }
        slot.Complete(true, {});
    }

    std::string ServerCapabilitiesJson() const override { return capabilities_; }
    std::string Instructions() const override { return instructions_; }

    // Asks the child's whole process group to exit and hands the child to the
    // caller (who gives it its grace period and, past that, kills it). The
    // reader then sees EOF, every pending request fails at once, and the worker
    // unwinds. Only a live process that is still OUR child is signalled.
    std::shared_ptr<sidecar::Child> Abort() override {
        std::shared_ptr<sidecar::Child> child;
        {
            std::lock_guard<std::mutex> lock(child_mu_);
            child = std::move(child_);
        }
        if (child) child->Signal(SIGTERM);
        return child;
    }

protected:
    bool Rpc(const std::string& method, const nlohmann::json& params, nlohmann::json* result,
             std::string* error) override {
        const std::int64_t id = next_id_++;
        auto pending = std::make_shared<Pending>();
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (!closed_reason_.empty()) {
                *error = closed_reason_;
                return false;
            }
            pending_[id] = pending;
        }
        std::string write_error;
        if (!WriteMessage({{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}},
                          write_error)) {
            std::lock_guard<std::mutex> lock(mu_);
            pending_.erase(id);
            *error = write_error;
            return false;
        }

        std::unique_lock<std::mutex> lock(mu_);
        const bool answered = cv_.wait_for(lock, request_timeout_, [&] {
            return pending->response.has_value() || !closed_reason_.empty();
        });
        pending_.erase(id);
        if (pending->response) {
            const nlohmann::json message = std::move(*pending->response);
            lock.unlock();
            return ReadResponse(message, result, error);
        }
        *error = answered ? closed_reason_ : "request timed out";
        return false;
    }

private:
    struct Pending {
        std::optional<nlohmann::json> response;  // guarded by StdioTransport::mu_
    };

    bool Start(std::string& error) {
        if (argv_.empty()) {
            error = "no command";
            return false;
        }
        // The child inherits the server's environment with the entry's env
        // layered over it, and its PATH decides where the command is found.
        sidecar::LaunchSpec spec;
        spec.env = sidecar::CurrentEnvironment();
        for (const auto& [key, value] : env_) spec.env[key] = value;
        spec.argv = argv_;
        const auto path = spec.env.find("PATH");
        const std::string resolved = sidecar::ResolveOnPath(
            argv_[0], path != spec.env.end() ? std::string_view(path->second) : std::string_view());
        if (resolved.empty()) {
            error = "'" + argv_[0] + "' not found on PATH";
            return false;
        }
        spec.argv[0] = resolved;
        std::error_code cwd_error;
        spec.cwd = std::filesystem::current_path(cwd_error);
        if (cwd_error) spec.cwd = "/";
        // stdout carries the protocol; stdin receives it. stderr is the
        // server's own, so a server's diagnostics land in the harness log.
        spec.pipes = {
            sidecar::PipePlan{sidecar::PipePlan::ChildEnd::kWrite, {{STDOUT_FILENO, false}}},
            sidecar::PipePlan{sidecar::PipePlan::ChildEnd::kRead, {{STDIN_FILENO, false}}},
        };
        spec.cpu_seconds = 0;
        spec.file_size_bytes = 0;

        sidecar::Launched launched = sidecar::Launch(spec);
        if (!launched.ok) {
            error = launched.spawn_error;
            return false;
        }
        stdout_ = std::move(launched.parent_fds[0]);
        stdin_ = std::move(launched.parent_fds[1]);
#ifdef F_SETNOSIGPIPE
        fcntl(stdin_.get(), F_SETNOSIGPIPE, 1);
#endif
        {
            std::lock_guard<std::mutex> lock(child_mu_);
            child_ = std::make_shared<sidecar::Child>(launched.pid);
        }
        reader_ = std::thread([this] { ReadLoop(); });
        return true;
    }

    bool WriteMessage(const nlohmann::json& message, std::string& error) {
        const std::string line = message.dump() + "\n";
        std::lock_guard<std::mutex> lock(write_mu_);
        std::size_t written = 0;
        while (written < line.size()) {
            const ssize_t n = ::write(stdin_.get(), line.data() + written, line.size() - written);
            if (n > 0) {
                written += static_cast<std::size_t>(n);
            } else if (n < 0 && errno == EINTR) {
                continue;
            } else {
                error = std::string("MCP transport error: writing to the server failed: ") +
                        std::strerror(errno);
                return false;
            }
        }
        return true;
    }

    void Close(std::string reason) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (closed_reason_.empty()) closed_reason_ = std::move(reason);
        }
        cv_.notify_all();
    }

    void Dispatch(const std::string& line) {
        nlohmann::json message = nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false);
        if (!message.is_object()) return;  // not JSON-RPC; servers print banners
        const bool has_id = message.contains("id") && !message.at("id").is_null();
        if (has_id && !message.contains("method")) {
            std::lock_guard<std::mutex> lock(mu_);
            for (auto& [id, pending] : pending_) {
                if (FrameIdMatches(message, id)) {
                    pending->response = std::move(message);
                    break;
                }
            }
            cv_.notify_all();
            return;
        }
        // A notification goes to the sink, which runs on this reader thread
        // and must not block: a slow sink stalls every in-flight request.
        // Server-originated REQUESTS demand a response this client does not
        // model and are ignored.
        if (!has_id && message.contains("method") && sink_) {
            try {
                sink_(message.dump());
            } catch (const std::exception& e) {
                common::DefaultLogger().warn("MCP notification sink threw: {}", e.what());
            }
        }
    }

    void ReadLoop() {
        std::string buffer;
        char chunk[sidecar::kPipeReadChunkBytes];
        while (!stopping_.load(std::memory_order_acquire)) {
            pollfd fd{stdout_.get(), POLLIN, 0};
            const int rc = ::poll(&fd, 1, static_cast<int>(common::kCancelPollSlice.count()));
            if (rc < 0 && errno == EINTR) continue;
            if (rc < 0) {
                Close("MCP transport error: polling the server failed");
                return;
            }
            if (rc == 0) continue;
            const ssize_t n = ::read(stdout_.get(), chunk, sizeof(chunk));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                Close("MCP server closed its output (the process exited?)");
                return;
            }
            buffer.append(chunk, static_cast<std::size_t>(n));
            std::size_t start = 0;
            for (std::size_t end; (end = buffer.find('\n', start)) != std::string::npos;
                 start = end + 1) {
                if (end > start) Dispatch(buffer.substr(start, end - start));
            }
            buffer.erase(0, start);
            if (buffer.size() > kMaxStdioLineBytes) {
                Close("MCP server sent an unterminated line over " +
                      std::to_string(kMaxStdioLineBytes) + " bytes; connection dropped");
                return;
            }
        }
    }

    std::vector<std::string> argv_;
    std::map<std::string, std::string> env_;  // pre-interpolated; never logged
    std::string client_name_;
    std::string client_version_;
    NotificationSink sink_;
    std::chrono::milliseconds request_timeout_;

    sidecar::ScopedFd stdin_;
    sidecar::ScopedFd stdout_;
    // The stdio server process. Held until Abort() hands it to the teardown;
    // see ~StdioTransport for the never-aborted case.
    std::mutex child_mu_;
    std::shared_ptr<sidecar::Child> child_;
    std::int64_t next_id_ = 1;  // worker thread only
    std::string capabilities_ = "{}";
    std::string instructions_;

    std::mutex write_mu_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::map<std::int64_t, std::shared_ptr<Pending>> pending_;  // guarded by mu_
    std::string closed_reason_;                                 // guarded by mu_
    std::atomic<bool> stopping_{false};
    std::thread reader_;
};

// --- http transport: minimal Streamable HTTP over HttpClient ---------------

class HttpTransport final : public JsonRpcTransport {
public:
    HttpTransport(const McpEndpoint& endpoint, std::chrono::milliseconds request_timeout,
                  std::int64_t max_list_pages)
        : JsonRpcTransport(max_list_pages), endpoint_(endpoint), request_timeout_(request_timeout) {}

    void Initialize(Slot<bool>& slot) override {
        nlohmann::json result;
        std::string error;
        if (!Rpc("initialize", InitializeParams(kClientName, kClientVersion), &result, &error)) {
            slot.Complete(std::nullopt, error);
            return;
        }
        // The session id rides the initialize response headers and must be
        // echoed on every later request.
        session_id_ = response_session_id_;
        if (!Notify("notifications/initialized", error)) {
            slot.Complete(std::nullopt, error);
            return;
        }
        slot.Complete(true, {});
    }

protected:
    // Sends one request and extracts the JSON-RPC response with the matching
    // id -- from a JSON body, or from an SSE stream that is aborted the moment
    // the response frame arrives (a server may otherwise keep the stream open
    // long after the answer).
    bool Rpc(const std::string& method, const nlohmann::json& params, nlohmann::json* result,
             std::string* error) override {
        return RpcWithId(method, params, NextId(), result, error);
    }

private:
    int64_t NextId() { return next_id_++; }

    net::HttpRequest BuildRequest(const std::string& body) const {
        net::HttpRequest request;
        request.url = endpoint_.url;
        request.body = body;
        request.request_timeout = request_timeout_;
        for (const auto& [key, value] : endpoint_.headers) {
            request.headers.push_back({key, value});
        }
        request.headers.push_back({"Content-Type", "application/json"});
        // Both content types must be accepted: a server may answer a POST
        // with a plain JSON body or with an SSE stream (the python SDK
        // answers SSE).
        request.headers.push_back({"Accept", "application/json, text/event-stream"});
        request.headers.push_back({"MCP-Protocol-Version", kProtocolVersion});
        if (!session_id_.empty()) {
            request.headers.push_back({"mcp-session-id", session_id_});
        }
        return request;
    }

    bool RpcWithId(const std::string& method, const nlohmann::json& params, int64_t id,
                   nlohmann::json* result, std::string* error) {
        const std::string body = nlohmann::json{{"jsonrpc", "2.0"},
                                                {"id", id},
                                                {"method", method},
                                                {"params", params}}
                                    .dump();

        auto slot = std::make_shared<Slot<nlohmann::json>>();
        auto status = std::make_shared<int>(0);
        auto is_sse = std::make_shared<bool>(false);
        auto session_id = std::make_shared<std::string>();
        auto buffer = std::make_shared<std::string>();
        auto parser = std::make_shared<net::SseParser>();
        // Exceptions must never escape into libcurl's C call stack (undefined
        // behavior) -- caught in the chunk sink, stashed here, and converted
        // to the error path once StreamPost has fully unwound.
        auto pending_error = std::make_shared<std::exception_ptr>();

        try {
            session_.StreamPost(
                BuildRequest(body),
                [status, is_sse, session_id](int code,
                                             const std::vector<net::HttpHeader>& headers) {
                    *status = code;
                    // Names arrive verbatim from the wire (libcurl does not
                    // normalize case): compare case-insensitively, the same
                    // rule net::FindHeaderCI is built on.
                    for (const auto& header : headers) {
                        if (net::HeaderNameEquals(header.name, "Content-Type")) {
                            *is_sse = header.value.find("text/event-stream") != std::string::npos;
                        }
                        // A CR/LF in a value libcurl handed back verbatim would
                        // be re-sent as-is on every later request (BuildRequest
                        // below) -- header injection into our OWN outbound
                        // requests, from a server we do not otherwise trust.
                        // Dropped, not stripped-and-kept: the code already
                        // treats an empty session id as "none offered" (both
                        // here and at BuildRequest), so this degrades to that
                        // existing, well-defined path rather than forwarding a
                        // sanitized-but-still-adversarial value.
                        if (net::HeaderNameEquals(header.name, "mcp-session-id") &&
                            header.value.find('\r') == std::string::npos &&
                            header.value.find('\n') == std::string::npos) {
                            *session_id = header.value;
                        }
                    }
                },
                [slot, status, is_sse, session_id, buffer, parser, pending_error,
                 id](std::string_view chunk) -> bool {
                    if (*is_sse) {
                        // SSE: incremental parse, stop at the response frame.
                        // SseParser::Feed throws on oversized lines; that must
                        // not cross libcurl's C write callback, so it is
                        // stashed and the transfer aborted cleanly.
                        try {
                            for (const auto& frame : parser->Feed(chunk)) {
                                if (frame.data.empty()) continue;
                                nlohmann::json parsed =
                                    nlohmann::json::parse(frame.data, nullptr, false);
                                if (FrameIdMatches(parsed, id)) {
                                    slot->Complete(std::move(parsed), {});
                                    return false;  // abort the transfer: we have our answer
                                }
                            }
                        } catch (...) {
                            *pending_error = std::current_exception();
                            return false;
                        }
                        return true;
                    }
                    // Plain JSON: read to completion.
                    buffer->append(chunk);
                    return true;
                });
        } catch (const net::HttpError& e) {
            // The abort path lands here too; a completed slot means the
            // response was found, not that anything failed. Read under the
            // slot mutex: Complete() may have run on this thread, but the
            // protocol must not depend on that timing.
            std::lock_guard<std::mutex> lock(slot->mu);
            if (!slot->done) {
                *error = std::string("MCP transport error: ") + e.what();
                return false;
            }
        }

        // A parse throw stashed by the chunk sink surfaces here, once
        // StreamPost has unwound -- converted to the error path (it is not a
        // net::HttpError, so the catch above never sees it) rather than
        // rethrown: escaping Rpc would only reach the worker's catch-all,
        // leaving the slot incomplete and the caller burning its deadline.
        if (*pending_error) {
            try {
                std::rethrow_exception(*pending_error);
            } catch (const std::exception& e) {
                *error = std::string("MCP transport error: ") + e.what();
                return false;
            }
        }

        if (!session_id->empty()) response_session_id_ = *session_id;

        nlohmann::json message;
        {
            std::lock_guard<std::mutex> lock(slot->mu);
            if (slot->done) {
                message = *slot->value;
            } else if (!common::IsSuccessStatus(*status)) {
                *error = "MCP server returned HTTP " + std::to_string(*status) +
                         (buffer->empty() ? std::string()
                                  : ": " + buffer->substr(0, common::kMaxErrorSnippetBytes));
                return false;
            } else if (*is_sse) {
                // The stream ended within the deadline but the matching frame
                // never came: a dedicated message, not the generic
                // "unparseable response" the empty-buffer fallthrough would
                // produce.
                *error = "MCP server closed the stream without a response to request " +
                         std::to_string(id);
                return false;
            } else {
                message = nlohmann::json::parse(*buffer, nullptr, false);
                if (!message.is_object()) {
                    *error = "MCP server returned an unparseable response";
                    return false;
                }
            }
        }

        return ReadResponse(message, result, error);
    }

    bool Notify(const std::string& method, std::string& error) {
        const std::string body = nlohmann::json{{"jsonrpc", "2.0"},
                                                {"method", method},
                                                {"params", nlohmann::json::object()}}
                                    .dump();
        try {
            const net::HttpResponse response = session_.Post(BuildRequest(body));
            if (!common::IsSuccessStatus(response.status)) {
                error = "MCP server returned HTTP " + std::to_string(response.status) +
                        " for " + method;
                return false;
            }
            return true;
        } catch (const net::HttpError& e) {
            error = std::string("MCP transport error: ") + e.what();
            return false;
        }
    }

    // One persistent CURL handle for the connection's whole life. All calls
    // run on the single worker thread -- the session's not-thread-safe
    // contract holds by construction, and requests to one host reuse the
    // TCP+TLS connection instead of re-handshaking per call.
    net::HttpSession session_;
    McpEndpoint endpoint_;
    std::chrono::milliseconds request_timeout_;
    std::string session_id_;           // worker-thread only
    std::string response_session_id_;  // out-of-band: Rpc records what headers carried
    int64_t next_id_ = 1;
};

}  // namespace

// --- worker ----------------------------------------------------------------

struct McpConnection::WorkerState {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<std::function<void()>> jobs;
    bool stopped = false;
    // Set under mu immediately before Loop returns: Teardown waits on it to
    // JOIN the thread rather than detach (see McpConnection::Teardown).
    bool exited = false;

    bool Push(std::function<void()> job) {
        {
            std::lock_guard<std::mutex> lock(mu);
            if (stopped) return false;  // teardown: the caller completes its slot instead
            jobs.push_back(std::move(job));
        }
        cv.notify_all();
        return true;
    }

    // The thread lambda holds the shared_ptrs to this state and to the
    // transport, so even a detached exit after a wedged blocking call leaves
    // everything alive. Loop touches nothing else.
    void Loop() {
        std::unique_lock<std::mutex> lock(mu);
        for (;;) {
            cv.wait(lock, [this] { return stopped || !jobs.empty(); });
            if (jobs.empty()) {
                if (stopped) break;
                continue;
            }
            std::function<void()> job = std::move(jobs.front());
            jobs.pop_front();
            lock.unlock();
            try {
                job();
            } catch (const std::exception& e) {
                // A throwing job never completes its slot; the caller's
                // deadline is the backstop. Swallow-and-continue: an
                // exception escaping here would std::terminate the whole
                // server from this thread.
                minimal::common::DefaultLogger().error("MCP connection job threw: {}",
                                                       e.what());
            } catch (...) {
                minimal::common::DefaultLogger().error(
                    "MCP connection job threw a non-standard exception");
            }
            lock.lock();
        }
        exited = true;
        cv.notify_all();
    }
};

// Submits the job to the worker. When the worker is already stopped (teardown
// won the race), the job will never run -- so complete the slot with `error`
// immediately instead of leaving the caller to burn its full deadline waiting
// on a completion that cannot happen. The Notify wakes the WaitSlot loop, and
// the usual lock discipline holds: the slot mutex is released before the
// token's cv is notified.
template <typename Worker, typename T>
void PushOrComplete(Worker& worker, std::function<void()> job, Slot<T>& slot,
                    const tools::CancelTokenPtr& cancel, std::string error) {
    if (worker.Push(std::move(job))) return;
    slot.Complete(std::nullopt, std::move(error));
    TokenFor(cancel).Notify();
}

// --- McpConnection ----------------------------------------------------------

McpConnection::McpConnection(std::shared_ptr<WorkerState> worker,
                             std::shared_ptr<Transport> transport, std::thread loop)
    : worker_(std::move(worker)), transport_(std::move(transport)), loop_(std::move(loop)) {}

McpConnection::~McpConnection() { Teardown(); }

void McpConnection::Teardown() {
    if (!worker_) return;
    // Abort the transport BEFORE stopping the worker: killing the stdio child
    // is what lets a wedged blocking call end, so the worker and the child it
    // holds unwind instead of lingering.
    std::shared_ptr<sidecar::Child> child = transport_->Abort();
    bool joined = false;
    {
        std::lock_guard<std::mutex> lock(worker_->mu);
        worker_->stopped = true;
    }
    // Notify BEFORE waiting: the worker's Loop() parks on this same cv with
    // predicate (stopped || !jobs.empty()), and nothing else wakes it once
    // Abort() has already run (Abort only kills the child; it does not touch
    // this cv). Notifying after the wait_for below is a guaranteed miss, not
    // a race -- the wait always times out, `joined` is always false, and
    // every teardown takes the detach branch this grace window exists to
    // avoid.
    worker_->cv.notify_all();
    {
        std::unique_lock<std::mutex> lock(worker_->mu);
        // JOIN, never blind-detach, whenever the worker exits in time: a
        // detached worker that unwinds late deletes the transport on its own
        // thread, possibly after main() has returned. Abort() ends a stdio
        // call almost at once (the reader sees EOF and fails every pending
        // request), and an http call is bounded by its request timeout, so
        // the grace only has to cover a worker leaving a call in flight.
        joined = worker_->cv.wait_for(lock, kWorkerJoinGrace,
                                      [this] { return worker_->exited; });
    }
    if (loop_.joinable()) {
        if (joined) {
            loop_.join();
            // The child is ours to reap NOW, on this thread: nothing else
            // waits for it, and nothing outlives this call.
            if (child) child->TerminateAfter(kChildTerminateGrace);
        } else {
            // Wedged past the grace: detach, never join. The thread references
            // only shared_ptr-held state, so it exits safely whenever its
            // call returns. The child gets a detached escalation watcher,
            // since this thread is done waiting.
            if (child) {
                std::thread([child] { child->TerminateAfter(kChildTerminateGrace); }).detach();
            }
            loop_.detach();
        }
    }
    worker_.reset();
}

std::unique_ptr<McpConnection> McpConnection::Open(const McpEndpoint& endpoint,
                                                   const McpLimits& limits,
                                                   const std::string& client_name,
                                                   const std::string& client_version,
                                                   Failure& error,
                                                   const NotificationSink& sink) {
    std::shared_ptr<Transport> transport;
    if (endpoint.transport == McpTransport::Stdio) {
        if (endpoint.argv.empty() || endpoint.argv[0].empty()) {
            error.message = "MCP stdio endpoint has no command";
            return nullptr;
        }
        transport = std::make_shared<StdioTransport>(endpoint, client_name, client_version, sink,
                                                     limits.request, limits.max_list_pages);
    } else {
        if (endpoint.url.empty()) {
            error.message = "MCP http endpoint has no url";
            return nullptr;
        }
        transport = std::make_shared<HttpTransport>(endpoint, limits.request, limits.max_list_pages);
    }

    auto worker = std::make_shared<WorkerState>();
    std::thread loop([worker, transport] { worker->Loop(); });

    auto connection = std::unique_ptr<McpConnection>(new McpConnection(
        std::move(worker), transport, std::move(loop)));
    connection->request_timeout_ = limits.request;

    // The initialize handshake runs on the worker under the connect deadline.
    // No token: the handshake has no cancellation path, only the deadline.
    auto slot = std::make_shared<Slot<bool>>();
    auto token = std::make_shared<tools::CancelToken>();
    PushOrComplete(*connection->worker_,
                   [transport, slot, token] {
                       transport->Initialize(*slot);
                       TokenFor(token).Notify();
                   },
                   *slot, token, "MCP connection is shutting down");
    if (WaitSlot(*slot, token, std::chrono::steady_clock::now() + limits.connect,
                 error.message) != WaitResult::kCompleted) {
        if (error.message.empty()) error.message = "MCP initialize timed out";
        connection->Teardown();
        return nullptr;
    }
    return connection;
}

std::string McpConnection::ServerCapabilitiesJson() const {
    return transport_->ServerCapabilitiesJson();
}

std::string McpConnection::Instructions() const { return transport_->Instructions(); }

bool McpConnection::Ping(const tools::CancelTokenPtr& cancel, Failure& error) {
    auto slot = std::make_shared<Slot<bool>>();
    PushOrComplete(*worker_,
                   [transport = transport_, slot, cancel] {
                       transport->Ping(*slot);
                       TokenFor(cancel).Notify();
                   },
                   *slot, cancel, "MCP connection is shutting down");
    const bool completed = WaitSlot(*slot, cancel,
                                    std::chrono::steady_clock::now() + request_timeout_,
                                    error.message) == WaitResult::kCompleted;
    if (!completed) return false;
    std::lock_guard<std::mutex> lock(slot->mu);
    return slot->value.value_or(false);
}

std::optional<std::vector<McpConnection::ToolListing>> McpConnection::ListTools(
        const tools::CancelTokenPtr& cancel, Failure& error) {
    auto slot = std::make_shared<Slot<std::vector<ToolListing>>>();
    PushOrComplete(*worker_,
                   [transport = transport_, slot, cancel] {
                       transport->ListTools(*slot);
                       TokenFor(cancel).Notify();
                   },
                   *slot, cancel, "MCP connection is shutting down");
    if (WaitSlot(*slot, cancel, std::chrono::steady_clock::now() + request_timeout_,
                 error.message) != WaitResult::kCompleted) {
        return std::nullopt;
    }
    std::lock_guard<std::mutex> lock(slot->mu);
    return std::move(*slot->value);
}

McpCallOutcome McpConnection::CallTool(std::string_view tool_name, const nlohmann::json& arguments,
                                       const tools::CancelTokenPtr& cancel) {
    McpCallOutcome outcome;
    auto slot = std::make_shared<Slot<nlohmann::json>>();
    PushOrComplete(*worker_,
                   [transport = transport_, slot, cancel, name = std::string(tool_name),
                    arguments] {
                       transport->CallTool(name, arguments, *slot);
                       TokenFor(cancel).Notify();
                   },
                   *slot, cancel, "MCP connection is shutting down");
    std::string error;
    const WaitResult wait = WaitSlot(*slot, cancel,
                                     std::chrono::steady_clock::now() + request_timeout_, error);
    if (wait != WaitResult::kCompleted) {
        outcome.error = std::move(error);
        if (outcome.error.empty()) outcome.error = "MCP call failed";
        // The typed form of the cancel case (was: error == "cancelled").
        outcome.caller_cancelled = wait == WaitResult::kCallerCancelled;
        return outcome;
    }
    {
        std::lock_guard<std::mutex> lock(slot->mu);
        outcome.result = std::move(*slot->value);
    }
    outcome.ok = true;
    return outcome;
}

}  // namespace minimal::mcp
