#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

#include "minimal/mcp/mcp_connection.hpp"
#include "support/deadline.hpp"

// cpp-mcp ships httplib.h for its own transports; the test reuses it as an
// in-process streamable-http SERVER fixture. SYSTEM include: third-party.
#include <httplib.h>

using minimal::mcp::McpCallOutcome;
using minimal::mcp::McpConnection;
using minimal::mcp::McpEndpoint;
using minimal::mcp::McpTimeouts;
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
                                         McpTimeouts timeouts = {std::chrono::seconds(10),
                                                                 std::chrono::seconds(10)}) {
    McpConnection::Failure error;
    auto connection = McpConnection::Open(StdioEndpoint(mode), timeouts, "test", "0.0.1", error);
    if (!connection) FAIL(error.message);
    return connection;
}

// One in-process streamable-http MCP server, scripted per test. deterministic:
// binds an ephemeral localhost port.
class FakeHttpMcpServer {
public:
    explicit FakeHttpMcpServer(bool reply_sse = true) : reply_sse_(reply_sse) {
        server_.Post("/mcp", [this](const httplib::Request& req, httplib::Response& res) {
            ++requests_;
            const nlohmann::json body = nlohmann::json::parse(req.body, nullptr, false);
            if (!body.is_object()) {
                res.status = 400;
                return;
            }
            const std::string method = body.value("method", "");

            if (method == "initialize") {
                session_id_ = "session-" + std::to_string(session_counter_++);
                res.set_header("mcp-session-id", session_id_);
                nlohmann::json result = {
                    {"protocolVersion", "2025-03-26"},
                    {"capabilities", nlohmann::json::object()},
                    {"serverInfo", {{"name", "fake-http"}, {"version", "0.0.1"}}}};
                Reply(res, body, result);
                return;
            }
            if (method == "notifications/initialized") {
                if (req.get_header_value("mcp-session-id") != session_id_) {
                    res.status = 400;  // session id not echoed
                    return;
                }
                res.status = 202;
                return;
            }
            // Everything after initialize requires the echoed session id.
            if (req.get_header_value("mcp-session-id") != session_id_) {
                res.status = 400;
                return;
            }
            if (method == "tools/list") {
                nlohmann::json result = {
                    {"tools",
                     nlohmann::json::array({nlohmann::json{
                         {"name", "echo"},
                         {"description", "Echo"},
                         {"inputSchema", nlohmann::json{{"type", "object"}}}}})}};
                Reply(res, body, result);
                return;
            }
            if (method == "tools/call") {
                if (body.at("params").value("name", "") == "boom") {
                    ReplyError(res, body, nlohmann::json{{"code", -32000},
                                                          {"message", "scripted rpc error"}});
                    return;
                }
                nlohmann::json result = {
                    {"content", nlohmann::json::array({nlohmann::json{
                                    {"type", "text"}, {"text", "http-echo"}}})},
                    {"isError", false}};
                Reply(res, body, result);
                return;
            }
            res.status = 404;
        });

        port_ = server_.bind_to_any_port("127.0.0.1");
        thread_ = std::thread([this] { server_.listen_after_bind(); });
        // listen_after_bind returns once accepting; give the socket a beat.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    ~FakeHttpMcpServer() {
        server_.stop();
        if (thread_.joinable()) thread_.join();
    }

    std::string url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/mcp"; }
    int requests() const { return requests_; }

private:
    // SSE-framed reply (the shape the python SDK answers) or plain JSON.
    void Reply(httplib::Response& res, const nlohmann::json& body,
               const nlohmann::json& result) const {
        nlohmann::json message = {{"jsonrpc", "2.0"}, {"id", body.at("id")}, {"result", result}};
        if (reply_sse_) {
            res.set_content("event: message\ndata: " + message.dump() + "\n\n",
                            "text/event-stream");
        } else {
            res.set_content(message.dump(), "application/json");
        }
    }

    void ReplyError(httplib::Response& res, const nlohmann::json& body,
                    const nlohmann::json& error) const {
        nlohmann::json message = {{"jsonrpc", "2.0"}, {"id", body.at("id")}, {"error", error}};
        if (reply_sse_) {
            res.set_content("event: message\ndata: " + message.dump() + "\n\n",
                            "text/event-stream");
        } else {
            res.set_content(message.dump(), "application/json");
        }
    }

    httplib::Server server_;
    std::thread thread_;
    int port_ = 0;
    int requests_ = 0;
    std::string session_id_;
    int session_counter_ = 0;
    bool reply_sse_ = true;
};

}  // namespace

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
    const auto run = RunInBackground([&] {
        return connection->CallTool("slow", nlohmann::json{{"ms", 10000}}, [] { return true; });
    });
    CHECK(run.WaitFor(std::chrono::seconds(2)));  // promptly: not after the 10 s tool
    if (run.Ready()) {
        const McpCallOutcome outcome = run.Get();
        CHECK_FALSE(outcome.ok);
        CHECK(outcome.error == "cancelled");
    }
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
    FakeHttpMcpServer server(/*reply_sse=*/true);
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
    FakeHttpMcpServer server(/*reply_sse=*/false);
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
