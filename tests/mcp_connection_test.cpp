#include <catch2/catch_test_macros.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

#include "minimal/mcp/mcp_connection.hpp"
#include "support/deadline.hpp"
#include "support/fake_http_mcp_server.hpp"

// cpp-mcp ships httplib.h for its own transports; the test reuses it as an
// in-process streamable-http SERVER fixture. SYSTEM include: third-party.
#include <httplib.h>

using minimal::mcp::McpCallOutcome;
using minimal::mcp::McpConnection;
using minimal::mcp::McpEndpoint;
using minimal::mcp::McpLimits;
using minimal::mcp::McpTransport;
using minimal::testing::RunInBackground;

namespace {

constexpr const char* kFakeServer = MINIMAL_MCP_FAKE_SERVER;

McpEndpoint StdioEndpoint(const std::string& mode) {
    McpEndpoint endpoint;
    endpoint.transport = McpTransport::Stdio;
    endpoint.command_line = std::string(kFakeServer) + " " + mode;
    return endpoint;
}

// A connection whose background jobs must finish; used for the happy paths.
std::unique_ptr<McpConnection> OpenStdio(const std::string& mode,
                                         McpLimits timeouts = {std::chrono::seconds(10),
                                                                 std::chrono::seconds(10)}) {
    McpConnection::Failure error;
    auto connection = McpConnection::Open(StdioEndpoint(mode), timeouts, "test", "0.0.1", error);
    if (!connection) FAIL(error.message);
    return connection;
}

}  // namespace

// A hand-framed HTTP server whose reason to exist is counting ACCEPTED
// CONNECTIONS -- the witness for connection reuse, which httplib cannot
// expose. Speaks the same plain-JSON MCP script (initialize, notification,
// tools/list) with keep-alive semantics: HTTP/1.1, no Connection: close, one
// response per request, several requests per connection.
class CountingRawHttpServer {
public:
    CountingRawHttpServer() {
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(listener_ >= 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        REQUIRE(::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        REQUIRE(::listen(listener_, 4) == 0);
        socklen_t length = sizeof(address);
        REQUIRE(::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length) == 0);
        port_ = ntohs(address.sin_port);

        thread_ = std::thread([this] { AcceptLoop(); });
    }

    ~CountingRawHttpServer() {
        ::close(listener_);
        if (thread_.joinable()) thread_.join();
    }

    std::string url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/mcp"; }
    int connections() const { return connections_; }

private:
    static void ReadFull(int fd, std::string& data, std::size_t want) {
        char buffer[4096];
        while (data.size() < want) {
            const ssize_t n = ::read(fd, buffer, sizeof(buffer));
            if (n <= 0) return;
            data.append(buffer, static_cast<std::size_t>(n));
        }
    }

    // Reads one request (headers + Content-Length body) off the connection.
    static bool ReadRequest(int fd, std::string& body) {
        std::string data;
        char buffer[4096];
        while (data.find("\r\n\r\n") == std::string::npos) {
            const ssize_t n = ::read(fd, buffer, sizeof(buffer));
            if (n <= 0) return false;
            data.append(buffer, static_cast<std::size_t>(n));
        }
        const std::size_t body_start = data.find("\r\n\r\n") + 4;
        std::size_t content_length = 0;
        for (std::size_t i = 0; i < body_start;) {
            const std::size_t eol = data.find("\r\n", i);
            if (eol == std::string::npos || eol > body_start) break;
            const std::string line = data.substr(i, eol - i);
            const std::size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string name = line.substr(0, colon);
                for (auto& c : name) c = static_cast<char>(::tolower(c));
                if (name == "content-length") content_length = std::stoul(line.substr(colon + 1));
            }
            i = eol + 2;
        }
        ReadFull(fd, data, body_start + content_length);
        if (data.size() < body_start + content_length) return false;
        body = data.substr(body_start, content_length);
        return true;
    }

    static void WriteAll(int fd, const std::string& text) {
        std::size_t sent = 0;
        while (sent < text.size()) {
            const ssize_t n = ::write(fd, text.data() + sent, text.size() - sent);
            if (n <= 0) return;
            sent += static_cast<std::size_t>(n);
        }
    }

    std::string ResponseFor(const std::string& body) const {
        const nlohmann::json request = nlohmann::json::parse(body, nullptr, false);
        const std::string method = request.value("method", "");
        // The initialized notification carries no id and expects no body --
        // a bare 202 satisfies Notify().
        if (!request.contains("id")) {
            return "HTTP/1.1 202 Accepted\r\nContent-Length: 0\r\n\r\n";
        }
        nlohmann::json result = nlohmann::json::object();
        if (method == "initialize") {
            result = {{"protocolVersion", "2025-03-26"},
                      {"capabilities", nlohmann::json::object()},
                      {"serverInfo", {{"name", "raw"}, {"version", "0.0.1"}}}};
        } else if (method == "tools/list") {
            result = {{"tools", nlohmann::json::array({nlohmann::json{
                                    {"name", "echo"}, {"description", "Echo"},
                                    {"inputSchema", nlohmann::json{{"type", "object"}}}}})}};
        }
        nlohmann::json message = {{"jsonrpc", "2.0"}, {"id", request.at("id")},
                                  {"result", result}};
        const std::string payload = message.dump();
        return "HTTP/1.1 200 OK\r\n"
               "Content-Type: application/json\r\n"
               "mcp-session-id: raw-session-1\r\n"
               "Content-Length: " +
               std::to_string(payload.size()) + "\r\n\r\n" + payload;
    }

    void AcceptLoop() {
        for (;;) {
            const int fd = ::accept(listener_, nullptr, nullptr);
            if (fd < 0) return;
            ++connections_;
            // Serve several requests over this one connection: keep-alive.
            for (int served = 0; served < 8; ++served) {
                std::string body;
                if (!ReadRequest(fd, body)) break;
                WriteAll(fd, ResponseFor(body));
            }
            ::close(fd);
        }
    }

    int listener_ = -1;
    int port_ = 0;
    std::thread thread_;
    std::atomic<int> connections_{0};
};

TEST_CASE("McpConnection http: one persistent connection rides the whole protocol", "[mcp_connection]") {
    // Open (initialize + notification) and tools/list are three separate
    // requests. The connection's HttpSession reuses its CURL handle, so they
    // must share ONE TCP connection -- the keep-alive win that per-call
    // handles (HttpClient) never had.
    CountingRawHttpServer server;
    McpEndpoint endpoint;
    endpoint.transport = McpTransport::Http;
    endpoint.url = server.url();

    McpConnection::Failure error;
    auto connection = McpConnection::Open(endpoint, {std::chrono::seconds(5), std::chrono::seconds(5)},
                                          "test", "0.0.1", error);
    REQUIRE(connection != nullptr);
    const auto tools = connection->ListTools(nullptr, error);
    REQUIRE(tools.has_value());
    CHECK((*tools)[0].name == "echo");
    CHECK(server.connections() == 1);
}

TEST_CASE("McpConnection stdio: initialize, discovery, ping, tool call", "[mcp_connection]") {
    auto connection = OpenStdio("normal");

    McpConnection::Failure error;
    CHECK(connection->Ping(nullptr, error));

    const auto tools = connection->ListTools(nullptr, error);
    REQUIRE(tools.has_value());
    REQUIRE(tools->size() == 4);
    CHECK((*tools)[0].name == "echo");
    CHECK((*tools)[0].input_schema.is_object());

    const McpCallOutcome outcome = connection->CallTool(
        "echo", nlohmann::json{{"text", "hello mcp"}}, nullptr);
    REQUIRE(outcome.ok);
    CHECK(outcome.result.at("content")[0].at("text") == "hello mcp");
    CHECK_FALSE(outcome.result.value("isError", true));
}

TEST_CASE("McpConnection stdio: a tool error surfaces as an isError result", "[mcp_connection]") {
    auto connection = OpenStdio("normal");
    const McpCallOutcome outcome = connection->CallTool("fail", nlohmann::json::object(), nullptr);
    // cpp-mcp wraps a throwing tool handler in a JSON-RPC *result* carrying
    // isError -- a protocol-valid response, not a transport failure. The
    // adapter maps that to is_error one layer up.
    REQUIRE(outcome.ok);
    CHECK(outcome.result.value("isError", true));
}

TEST_CASE("McpConnection stdio: slow tool honours the request timeout", "[mcp_connection]") {
    // 2 s request timeout against a tool that sleeps 10 s: the outcome must
    // arrive within the deadline harness bound, never hang.
    auto connection = OpenStdio("normal", {std::chrono::seconds(10), std::chrono::seconds(2)});
    const auto run = RunInBackground([&] {
        return connection->CallTool("slow", nlohmann::json{{"ms", 10000}}, nullptr);
    });
    CHECK(run.WaitFor(std::chrono::seconds(5)));
    if (run.Ready()) {
        const McpCallOutcome outcome = run.Get();
        CHECK_FALSE(outcome.ok);
        CHECK(outcome.error.find("timed out") != std::string::npos);
    }
}

TEST_CASE("McpConnection stdio: slow tool succeeds under a generous timeout", "[mcp_connection]") {
    auto connection = OpenStdio("normal", {std::chrono::seconds(10), std::chrono::seconds(10)});
    const McpCallOutcome outcome =
        connection->CallTool("slow", nlohmann::json{{"ms", 200}}, nullptr);
    CHECK(outcome.ok);
    CHECK(outcome.result.at("content")[0].at("text") == "done");
}

TEST_CASE("McpConnection stdio: cancellation returns promptly with an error", "[mcp_connection]") {
    auto connection = OpenStdio("normal", {std::chrono::seconds(10), std::chrono::seconds(10)});
    auto cancelled = std::make_shared<minimal::tools::CancelToken>();
    cancelled->Cancel();
    const auto run = RunInBackground([&] {
        return connection->CallTool("slow", nlohmann::json{{"ms", 10000}}, cancelled);
    });
    CHECK(run.WaitFor(std::chrono::seconds(2)));  // promptly: not after the 10 s tool
    if (run.Ready()) {
        const McpCallOutcome outcome = run.Get();
        CHECK_FALSE(outcome.ok);
        CHECK(outcome.error == "cancelled");
    }
}

TEST_CASE("McpConnection stdio: cancelling mid-call wakes the blocked waiter", "[mcp_connection]") {
    // The event-driven contract: a tool blocked on the token's cv wakes the
    // moment the token fires -- not on a poll slice, not after the deadline.
    auto connection = OpenStdio("hang", {std::chrono::seconds(10), std::chrono::seconds(30)});
    auto token = std::make_shared<minimal::tools::CancelToken>();
    const auto run = RunInBackground([&] {
        return connection->CallTool("hang", nlohmann::json::object(), token);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));  // let the call park
    token->Cancel();
    CHECK(run.WaitFor(std::chrono::seconds(2)));  // promptly: not after 30 s
    if (run.Ready()) {
        const McpCallOutcome outcome = run.Get();
        CHECK_FALSE(outcome.ok);
        CHECK(outcome.error == "cancelled");
    }
    // The wedged stdio child would outlive everything without teardown's
    // kill; the destructor handles it.
}

TEST_CASE("McpConnection stdio: hanging tool degrades to a bounded error", "[mcp_connection]") {
    auto connection = OpenStdio("hang", {std::chrono::seconds(10), std::chrono::milliseconds(400)});
    const auto run = RunInBackground(
        [&] { return connection->CallTool("hang", nlohmann::json::object(), nullptr); });
    CHECK(run.WaitFor(std::chrono::seconds(5)));
    if (run.Ready()) {
        const McpCallOutcome outcome = run.Get();
        CHECK_FALSE(outcome.ok);
        CHECK(outcome.error.find("timed out") != std::string::npos);
    }
}

TEST_CASE("McpConnection stdio: server dying mid-life degrades to a bounded error", "[mcp_connection]") {
    // 2 s: comfortably after the initialize handshake (~0.5-1 s), so Open sees a
    // LIVE server that then dies -- the state under test. 500 ms was flaky:
    // the child sometimes died mid-handshake.
    auto connection = OpenStdio("die_after_ms:2000", {std::chrono::seconds(10), std::chrono::seconds(10)});
    std::this_thread::sleep_for(std::chrono::seconds(3));
    const auto run = RunInBackground([&] {
        McpConnection::Failure error;
        return connection->Ping(nullptr, error);
    });
    CHECK(run.WaitFor(std::chrono::seconds(5)));
    if (run.Ready()) {
        CHECK_FALSE(run.Get());  // dead server: false, not a hang
    }
}

TEST_CASE("McpConnection stdio: teardown kills a wedged call's child process", "[mcp_connection]") {
    // The hang tool parks the worker inside cpp-mcp's blocking call well past
    // every deadline; the child used to linger for cpp-mcp's internal 60 s
    // (or forever, had the server process exited first). Teardown must kill
    // it: the fake server writes its pid to a file, and the test -- being the
    // child's parent -- observes the death via waitpid.
    char pid_file[] = "/tmp/minimal_fake_mcp_pid_XXXXXX";
    const int pid_fd = mkstemp(pid_file);
    REQUIRE(pid_fd != -1);
    close(pid_fd);  // only the name matters; the child writes the file

    McpEndpoint endpoint = StdioEndpoint("hang");
    endpoint.env["MINIMAL_FAKE_PID_FILE"] = pid_file;
    McpConnection::Failure error;
    auto connection = McpConnection::Open(endpoint, {std::chrono::seconds(10), std::chrono::seconds(1)},
                                          "test", "0.0.1", error);
    REQUIRE(connection != nullptr);

    // Read the pid the child reported, then stop the caller's deadline from
    // mattering: the call itself times out at 1 s while the child hangs on.
    int child_pid = -1;
    for (int i = 0; i < 100 && child_pid <= 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::ifstream in(pid_file);
        int pid = -1;
        if (in >> pid) child_pid = pid;
    }
    REQUIRE(child_pid > 0);

    const auto run = RunInBackground([&] {
        return connection->CallTool("hang", nlohmann::json::object(), nullptr);
    });
    CHECK(run.WaitFor(std::chrono::seconds(5)));

    // Teardown (the destructor) while the worker is still wedged inside the
    // hung call: the child must die promptly -- not at cpp-mcp's internal
    // 60 s cap.
    connection.reset();

    bool child_dead = false;
    for (int i = 0; i < 40; ++i) {  // 40 * 100 ms = 4 s bound
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        int status = 0;
        // The test process is the child's parent: 0 = still alive, pid =
        // dead (zombie or reaped by the shim's own destructor path), ECHILD
        // = already reaped by that path. All but 0 prove death.
        const pid_t result = waitpid(child_pid, &status, WNOHANG);
        if (result != 0) {
            child_dead = true;
            break;
        }
    }
    CHECK(child_dead);
    std::remove(pid_file);
}

TEST_CASE("McpConnection stdio: garbage protocol output fails Open within the deadline", "[mcp_connection]") {
    // BackgroundResult needs a copyable T, so Open runs entirely on the
    // background thread and only its verdict crosses back.
    const auto run = RunInBackground([] {
        McpConnection::Failure error;
        auto connection = McpConnection::Open(StdioEndpoint("garbage"),
                                              {std::chrono::seconds(2), std::chrono::seconds(5)},
                                              "test", "0.0.1", error);
        return connection == nullptr;
    });
    CHECK(run.WaitFor(std::chrono::seconds(6)));
    if (run.Ready()) {
        CHECK(run.Get());  // bounded failure: Open returned null within the deadline
    }
}

TEST_CASE("McpConnection http: SSE-framed responses drive the full protocol", "[mcp_connection]") {
    FakeHttpMcpServer server(FakeHttpMcpServer::Options{.reply_sse = true});
    McpEndpoint endpoint;
    endpoint.transport = McpTransport::Http;
    endpoint.url = server.url();

    McpConnection::Failure error;
    auto connection = McpConnection::Open(endpoint, {std::chrono::seconds(5), std::chrono::seconds(5)},
                                          "test", "0.0.1", error);
    REQUIRE(connection != nullptr);

    const auto tools = connection->ListTools(nullptr, error);
    REQUIRE(tools.has_value());
    REQUIRE(tools->size() == 1);
    CHECK((*tools)[0].name == "echo");

    const McpCallOutcome outcome = connection->CallTool("echo", nlohmann::json::object(), nullptr);
    REQUIRE(outcome.ok);
    CHECK(outcome.result.at("content")[0].at("text") == "http-echo");
}

TEST_CASE("McpConnection http: plain JSON responses work too", "[mcp_connection]") {
    FakeHttpMcpServer server(FakeHttpMcpServer::Options{.reply_sse = false});
    McpEndpoint endpoint;
    endpoint.transport = McpTransport::Http;
    endpoint.url = server.url();

    McpConnection::Failure error;
    auto connection = McpConnection::Open(endpoint, {std::chrono::seconds(5), std::chrono::seconds(5)},
                                          "test", "0.0.1", error);
    REQUIRE(connection != nullptr);
    const McpCallOutcome outcome = connection->CallTool("echo", nlohmann::json::object(), nullptr);
    REQUIRE(outcome.ok);
}

TEST_CASE("McpConnection http: a JSON-RPC error response is a failed call, not a throw", "[mcp_connection]") {
    FakeHttpMcpServer server;
    McpEndpoint endpoint;
    endpoint.transport = McpTransport::Http;
    endpoint.url = server.url();

    McpConnection::Failure error;
    auto connection = McpConnection::Open(endpoint, {std::chrono::seconds(5), std::chrono::seconds(5)},
                                          "test", "0.0.1", error);
    REQUIRE(connection != nullptr);
    const McpCallOutcome outcome = connection->CallTool("boom", nlohmann::json::object(), nullptr);
    CHECK_FALSE(outcome.ok);
    CHECK(outcome.error.find("scripted rpc error") != std::string::npos);
}

TEST_CASE("McpConnection http: tools/list pagination is followed", "[mcp_connection]") {
    FakeHttpMcpServer server(FakeHttpMcpServer::Options{.reply_sse = false, .list_pages = 3});
    McpEndpoint endpoint;
    endpoint.transport = McpTransport::Http;
    endpoint.url = server.url();

    McpConnection::Failure error;
    auto connection = McpConnection::Open(endpoint, {std::chrono::seconds(5), std::chrono::seconds(5)},
                                          "test", "0.0.1", error);
    REQUIRE(connection != nullptr);
    const auto tools = connection->ListTools(nullptr, error);
    REQUIRE(tools.has_value());
    REQUIRE(tools->size() == 3);
    CHECK((*tools)[0].name == "echo");
    CHECK((*tools)[2].name == "echo-2");
    CHECK(server.requests() == 5);  // initialize + initialized + 3 pages
}

TEST_CASE("McpConnection http: a server that never stops paginating hits the cap", "[mcp_connection]") {
    FakeHttpMcpServer server(FakeHttpMcpServer::Options{.reply_sse = false, .list_pages = 1000000});
    McpEndpoint endpoint;
    endpoint.transport = McpTransport::Http;
    endpoint.url = server.url();

    McpConnection::Failure error;
    auto connection = McpConnection::Open(endpoint, {std::chrono::seconds(5), std::chrono::seconds(5), 2},
                                          "test", "0.0.1", error);
    REQUIRE(connection != nullptr);
    const auto tools = connection->ListTools(nullptr, error);
    CHECK_FALSE(tools.has_value());
    CHECK(error.message.find("exceeded 2 pages") != std::string::npos);
    CHECK(error.message.find("max_list_pages") != std::string::npos);
    CHECK(server.requests() == 4);  // initialize + initialized + exactly 2 pages
}

TEST_CASE("McpConnection http: a stringified id echo still matches", "[mcp_connection]") {
    FakeHttpMcpServer server(FakeHttpMcpServer::Options{.reply_sse = true, .id_as_string = true});
    McpEndpoint endpoint;
    endpoint.transport = McpTransport::Http;
    endpoint.url = server.url();

    McpConnection::Failure error;
    // initialize itself rides the lenient match: the handshake must succeed.
    auto connection = McpConnection::Open(endpoint, {std::chrono::seconds(5), std::chrono::seconds(5)},
                                          "test", "0.0.1", error);
    REQUIRE(connection != nullptr);
    const auto tools = connection->ListTools(nullptr, error);
    REQUIRE(tools.has_value());
    const McpCallOutcome outcome = connection->CallTool("echo", nlohmann::json::object(), nullptr);
    REQUIRE(outcome.ok);
}

TEST_CASE("McpConnection http: a stream with no matching frame is a named error", "[mcp_connection]") {
    FakeHttpMcpServer server(
        FakeHttpMcpServer::Options{.reply_sse = true, .no_response_frame = true});
    McpEndpoint endpoint;
    endpoint.transport = McpTransport::Http;
    endpoint.url = server.url();

    McpConnection::Failure error;
    auto connection = McpConnection::Open(endpoint, {std::chrono::seconds(5), std::chrono::seconds(5)},
                                          "test", "0.0.1", error);
    REQUIRE(connection != nullptr);
    const auto tools = connection->ListTools(nullptr, error);
    CHECK_FALSE(tools.has_value());
    CHECK(error.message.find("closed the stream without a response") != std::string::npos);
}

TEST_CASE("McpConnection http: a silent server is bounded by the request timeout", "[mcp_connection]") {
    // The handler sleeps 2 s before answering; a 500 ms request timeout must
    // cap the transfer (the total cap applies in streaming mode too).
    FakeHttpMcpServer server(FakeHttpMcpServer::Options{.reply_sse = true, .call_delay_ms = 2000});
    McpEndpoint endpoint;
    endpoint.transport = McpTransport::Http;
    endpoint.url = server.url();

    McpConnection::Failure error;
    // 1 s request timeout against a 2 s handler: the total cap applies in
    // streaming mode too, so the transfer aborts at the deadline.
    auto connection = McpConnection::Open(endpoint, {std::chrono::seconds(5), std::chrono::seconds(1)},
                                          "test", "0.0.1", error);
    REQUIRE(connection != nullptr);
    const auto run = RunInBackground([&] {
        return connection->CallTool("echo", nlohmann::json::object(), nullptr);
    });
    CHECK(run.WaitFor(std::chrono::seconds(3)));  // promptly: not after the 2 s handler
    if (run.Ready()) {
        const McpCallOutcome outcome = run.Get();
        CHECK_FALSE(outcome.ok);
        // Either the caller deadline ("request timed out") or the transport's
        // total cap ("MCP transport error: Timeout was reached") — both
        // bounded, both a timeout.
        CHECK((outcome.error.find("timed") != std::string::npos ||
               outcome.error.find("Timeout") != std::string::npos));
    }
}
