#include "minimal/mcp/mcp_connection.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>

#include "minimal/mcp/mcp_stdio_shim.hpp"
#include "minimal/net/http_client.hpp"
#include "minimal/net/sse_parser.hpp"

namespace minimal::mcp {

namespace {

constexpr const char* kProtocolVersion = "2025-03-26";
constexpr const char* kClientName = "minimal";
constexpr const char* kClientVersion = "0.1.0";

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

// Waits until the slot completes, the token is cancelled, or the deadline
// passes. Zero polling: the completion path notifies the token's cv, so the
// waiter wakes on exactly the events that can change its outcome. True when
// the slot completed; false after setting `error` -- the underlying job keeps
// running in the background either way.
//
// Lock order: the token mutex is held across the predicate loop and the wait,
// then RELEASED before slot.mu is taken -- the worker's completion path takes
// them in the opposite order (slot.mu, then token.mu in Notify), and a
// waiter that held both would close a cycle.
template <typename T>
bool WaitSlot(Slot<T>& slot, const tools::CancelTokenPtr& cancel,
              std::chrono::steady_clock::time_point deadline, std::string& error) {
    tools::CancelToken& token = TokenFor(cancel);
    {
        std::unique_lock<std::mutex> lock(token.Mu());
        while (!slot.done.load(std::memory_order_acquire)) {
            if (token.IsCancelled()) {
                error = "cancelled";
                return false;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                error = "request timed out";
                return false;
            }
            token.Cv().wait_until(lock, deadline);
        }
    }
    std::lock_guard<std::mutex> slot_lock(slot.mu);
    if (slot.error.empty()) return true;
    error = slot.error;
    return false;
}

// We send integer ids; a conforming server echoes the id verbatim. Lenient
// anyway: an integer, or a string that parses to the same integer (some
// server SDKs stringify ids). Server-originated requests (notifications and
// their own request ids) must not match, and non-numeric strings cannot.
bool FrameIdMatches(const nlohmann::json& message, int64_t id) {
    if (!message.is_object() || !message.contains("id")) return false;
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
    // detection. Defaults fit a transport with no initialize-result capture
    // (http): no capabilities, no instructions. Not worker-thread work -- the
    // base contract holds that these are plain reads of state initialize
    // already wrote, and Open's slot-wait orders them.
    virtual std::string ServerCapabilitiesJson() const { return "{}"; }
    virtual std::string Instructions() const { return {}; }
    // Teardown support, called from the DESTROYING thread while the worker may
    // be wedged inside a blocking call above: unblock the underlying I/O the
    // only way it can be unblocked (kill the stdio child; the blocking read
    // then ends, cpp-mcp's 60 s request cap fires, and the detached worker
    // unwinds). A no-op for transports without a process to kill. Returns the
    // pid the caller must reap (ReapChild/WatchChildDetached), 0 for none.
    virtual int Abort() { return 0; }
};

namespace {

// --- stdio transport: cpp-mcp's client over a spawned subprocess ----------

class StdioTransport final : public McpConnection::Transport {
public:
    StdioTransport(const McpEndpoint& endpoint, const std::string& client_name,
                   const std::string& client_version, const NotificationSink& sink)
        : client_name_(client_name), client_version_(client_version) {
        // Env arrives pre-interpolated (secrets included) and is never logged.
        for (const auto& [key, value] : endpoint.env) env_[key] = value;
        shim_ = std::make_unique<McpStdioShim>(endpoint.command_line, env_);
        // Pre-initialize: the read thread that would invoke the sink starts
        // inside Initialize(), and the shim's sink capture is parked until
        // then. No sink = the pre-channels behavior, unchanged.
        if (sink) shim_->SetNotificationSink(sink);
    }

    void Initialize(Slot<bool>& slot) override {
        std::string error;
        if (shim_->Initialize(client_name_, client_version_, &error)) {
            slot.Complete(true, {});
        } else {
            slot.Complete(std::nullopt,
                          "MCP initialize failed: server rejected the handshake or died "
                          "during startup" +
                              (error.empty() ? std::string() : ": " + error));
        }
    }

    // Kill the child NOW, from the destroying thread: a wedged blocking call
    // would otherwise hold the child (and the detached worker's shared_ptrs)
    // for up to cpp-mcp's internal 60 s per request -- and an orphaned child
    // forever if the server process exits first.
    int Abort() override { return shim_->AbortChild(); }

    // Plain reads of initialize-result state, ordered by Open's slot-wait.
    std::string ServerCapabilitiesJson() const override {
        return shim_->ServerCapabilitiesJson();
    }
    std::string Instructions() const override { return shim_->Instructions(); }

    void Ping(Slot<bool>& slot) override { slot.Complete(std::optional<bool>(shim_->Ping()), {}); }

    void ListTools(Slot<std::vector<McpConnection::ToolListing>>& slot) override {
        std::vector<McpConnection::ToolListing> listings;
        for (const auto& tool : shim_->ListTools()) {
            McpConnection::ToolListing listing;
            listing.name = tool.name;
            listing.description = tool.description;
            listing.input_schema = nlohmann::json::parse(tool.schema_json, nullptr, false);
            if (!listing.input_schema.is_object()) listing.input_schema = nlohmann::json::object();
            listings.push_back(std::move(listing));
        }
        slot.Complete(std::move(listings), {});
    }

    void CallTool(const std::string& tool_name, const nlohmann::json& arguments,
                  Slot<nlohmann::json>& slot) override {
        std::string result_json, error;
        if (!shim_->CallTool(tool_name, arguments.dump(), result_json, error)) {
            slot.Complete(std::nullopt, error);
            return;
        }
        nlohmann::json parsed = nlohmann::json::parse(result_json, nullptr, false);
        if (!parsed.is_object()) {
            slot.Complete(std::nullopt, "MCP server returned an unparseable tool result");
            return;
        }
        slot.Complete(std::move(parsed), {});
    }

private:
    std::string client_name_;
    std::string client_version_;
    std::map<std::string, std::string> env_;
    std::unique_ptr<McpStdioShim> shim_;
};

// --- http transport: minimal Streamable HTTP over HttpClient ---------------

class HttpTransport final : public McpConnection::Transport {
public:
    HttpTransport(const McpEndpoint& endpoint, std::chrono::milliseconds request_timeout,
                  std::int64_t max_list_pages)
        : endpoint_(endpoint), request_timeout_(request_timeout), max_list_pages_(max_list_pages) {}

    void Initialize(Slot<bool>& slot) override {
        nlohmann::json params = {
            {"protocolVersion", kProtocolVersion},
            {"capabilities", nlohmann::json::object()},
            {"clientInfo", {{"name", kClientName}, {"version", kClientVersion}}}};
        nlohmann::json result;
        std::string error;
        if (!Rpc("initialize", params, NextId(), &result, &error)) {
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

    void Ping(Slot<bool>& slot) override {
        nlohmann::json result;
        std::string error;
        slot.Complete(std::optional<bool>(Rpc("ping", nlohmann::json::object(), NextId(),
                                              &result, &error)),
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
        // page is individually within request_timeout, so no deadline fires).
        for (std::int64_t page = 0;; ++page) {
            if (page >= max_list_pages_) {
                slot.Complete(std::nullopt,
                              "MCP tools/list exceeded " + std::to_string(max_list_pages_) +
                                  " pages (tools.mcp.max_list_pages); refusing further "
                                  "cursors from a possibly broken or malicious server");
                return;
            }
            nlohmann::json params = nlohmann::json::object();
            if (!cursor.empty()) params["cursor"] = cursor;
            nlohmann::json result;
            if (!Rpc("tools/list", params, NextId(), &result, &error)) {
                slot.Complete(std::nullopt, error);
                return;
            }
            if (!result.contains("tools") || !result.at("tools").is_array()) {
                slot.Complete(std::nullopt, "MCP tools/list response has no tools array");
                return;
            }
            for (const auto& tool : result.at("tools")) {
                McpConnection::ToolListing listing;
                listing.name = tool.value("name", "");
                listing.description = tool.value("description", "");
                listing.input_schema =
                    tool.contains("inputSchema") ? tool.at("inputSchema") : nlohmann::json::object();
                listings.push_back(std::move(listing));
            }
            cursor = result.value("nextCursor", std::string());
            if (cursor.empty()) break;
        }
        slot.Complete(std::move(listings), {});
    }

    void CallTool(const std::string& tool_name, const nlohmann::json& arguments,
                  Slot<nlohmann::json>& slot) override {
        nlohmann::json result;
        std::string error;
        if (!Rpc("tools/call", {{"name", tool_name}, {"arguments", arguments}}, NextId(),
                 &result, &error)) {
            slot.Complete(std::nullopt, error);
            return;
        }
        slot.Complete(std::move(result), {});
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

    // Sends one request and, for requests, extracts the JSON-RPC response
    // with the matching id -- from a JSON body, or from an SSE stream that is
    // aborted the moment the response frame arrives (a server may otherwise
    // keep the stream open long after the answer).
    bool Rpc(const std::string& method, const nlohmann::json& params, int64_t id,
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
                [slot, status, is_sse, session_id, buffer, parser,
                 id](std::string_view chunk) -> bool {
                    if (*is_sse) {
                        // SSE: incremental parse, stop at the response frame.
                        for (const auto& frame : parser->Feed(chunk)) {
                            if (frame.data.empty()) continue;
                            nlohmann::json parsed =
                                nlohmann::json::parse(frame.data, nullptr, false);
                            if (FrameIdMatches(parsed, id)) {
                                slot->Complete(std::move(parsed), {});
                                return false;  // abort the transfer: we have our answer
                            }
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

        if (!session_id->empty()) response_session_id_ = *session_id;

        nlohmann::json message;
        {
            std::lock_guard<std::mutex> lock(slot->mu);
            if (slot->done) {
                message = *slot->value;
            } else if (*status < 200 || *status >= 300) {
                *error = "MCP server returned HTTP " + std::to_string(*status) +
                         (buffer->empty() ? std::string() : ": " + buffer->substr(0, 200));
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

        if (message.contains("error")) {
            *error = "MCP server error: " + message.at("error").value("message", "unknown");
            return false;
        }
        *result = message.contains("result") ? message.at("result") : nlohmann::json::object();
        return true;
    }

    bool Notify(const std::string& method, std::string& error) {
        const std::string body = nlohmann::json{{"jsonrpc", "2.0"},
                                                {"method", method},
                                                {"params", nlohmann::json::object()}}
                                    .dump();
        try {
            const net::HttpResponse response = session_.Post(BuildRequest(body));
            if (response.status < 200 || response.status >= 300) {
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
    std::int64_t max_list_pages_;
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

    void Push(std::function<void()> job) {
        {
            std::lock_guard<std::mutex> lock(mu);
            if (stopped) return;  // teardown: the job's slot times out instead
            jobs.push_back(std::move(job));
        }
        cv.notify_all();
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
    const int child_pid = transport_->Abort();
    bool joined = false;
    {
        std::unique_lock<std::mutex> lock(worker_->mu);
        worker_->stopped = true;
        // JOIN, never blind-detach, whenever the worker exits in time. A
        // detached worker that unwinds late -- or never -- deletes the
        // transport (last shared_ptr) on its own thread, and cpp-mcp's
        // destructor path then logs through a function-local-static logger:
        // after main() returns that static's mutex is destroyed, and the
        // logging thread aborts the whole process on a destroyed-mutex lock
        // (observed under TSan, whose 4x slowdown widened the window that
        // the default build races through). The exit is bimodal, so the
        // grace is short: a worker parked on cv between calls exits in
        // microseconds, and one inside a call whose child was just killed
        // stays wedged on cpp-mcp's internal 60 s future wait -- there is
        // nothing in between worth waiting for, and Open's connect-deadline
        // failure path (dead child, wedged initialize) must return promptly,
        // not burn the grace on a wait that cannot succeed.
        joined = worker_->cv.wait_for(lock, std::chrono::milliseconds(250),
                                      [this] { return worker_->exited; });
    }
    worker_->cv.notify_all();
    if (loop_.joinable()) {
        if (joined) {
            loop_.join();
            // The child is ours to reap NOW, on this thread: the worker is
            // gone, cpp-mcp's own destructor waitpid will only ever see
            // ECHILD (it skips its 2 s SIGTERM grace on that), and nothing
            // outlives this call.
            McpStdioShim::ReapChild(child_pid);
        } else {
            // Wedged past the grace: detach, never join. The thread may be
            // parked inside cpp-mcp's blocking call with no deadline. It
            // references only shared_ptr-held state, so it exits safely
            // whenever that call returns. The child gets the detached
            // escalation watcher, since this thread is done waiting.
            McpStdioShim::WatchChildDetached(child_pid);
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
        if (endpoint.command_line.empty()) {
            error.message = "MCP stdio endpoint has no command";
            return nullptr;
        }
        transport = std::make_shared<StdioTransport>(endpoint, client_name, client_version, sink);
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
    // Bounded even when the subprocess dies during startup, which wedges
    // cpp-mcp's own initialize indefinitely (verified by probe). No token:
    // the handshake has no cancellation path, only the deadline.
    auto slot = std::make_shared<Slot<bool>>();
    auto token = std::make_shared<tools::CancelToken>();
    connection->worker_->Push([transport, slot, token] {
        transport->Initialize(*slot);
        TokenFor(token).Notify();
    });
    if (!WaitSlot(*slot, token, std::chrono::steady_clock::now() + limits.connect,
                  error.message)) {
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
    worker_->Push([transport = transport_, slot, cancel] {
        transport->Ping(*slot);
        TokenFor(cancel).Notify();
    });
    const bool completed =
        WaitSlot(*slot, cancel, std::chrono::steady_clock::now() + request_timeout_, error.message);
    if (!completed) return false;
    std::lock_guard<std::mutex> lock(slot->mu);
    return slot->value.value_or(false);
}

std::optional<std::vector<McpConnection::ToolListing>> McpConnection::ListTools(
        const tools::CancelTokenPtr& cancel, Failure& error) {
    auto slot = std::make_shared<Slot<std::vector<ToolListing>>>();
    worker_->Push([transport = transport_, slot, cancel] {
        transport->ListTools(*slot);
        TokenFor(cancel).Notify();
    });
    if (!WaitSlot(*slot, cancel, std::chrono::steady_clock::now() + request_timeout_,
                  error.message)) {
        return std::nullopt;
    }
    std::lock_guard<std::mutex> lock(slot->mu);
    return std::move(*slot->value);
}

McpCallOutcome McpConnection::CallTool(std::string_view tool_name, const nlohmann::json& arguments,
                                       const tools::CancelTokenPtr& cancel) {
    McpCallOutcome outcome;
    auto slot = std::make_shared<Slot<nlohmann::json>>();
    worker_->Push([transport = transport_, slot, cancel, name = std::string(tool_name),
                   arguments] {
        transport->CallTool(name, arguments, *slot);
        TokenFor(cancel).Notify();
    });
    std::string error;
    if (!WaitSlot(*slot, cancel, std::chrono::steady_clock::now() + request_timeout_, error)) {
        outcome.error = std::move(error);
        if (outcome.error.empty()) outcome.error = "MCP call failed";
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
