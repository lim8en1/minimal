#include <catch2/catch_test_macros.hpp>

#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
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

// One in-process streamable-http MCP server, scripted per test. deterministic:
// binds an ephemeral localhost port.
class FakeHttpMcpServer {
public:
    struct Options {
        bool reply_sse = true;
        int list_pages = 1;             // >1: tools/list answers with nextCursor pages
        bool id_as_string = false;      // echoes JSON-RPC ids as strings
        bool no_response_frame = false; // SSE reply carrying only unrelated frames
        int call_delay_ms = 0;          // tools/call sleeps before answering
    };

    FakeHttpMcpServer() : FakeHttpMcpServer(Options{}) {}
    explicit FakeHttpMcpServer(Options options) : options_(options) {
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
                const int page = static_cast<int>(list_pages_served_++);
                const bool more = page + 1 < options_.list_pages;
                const std::string tool_name =
                    page == 0 ? "echo" : "echo-" + std::to_string(page);
                nlohmann::json result = {
                    {"tools",
                     nlohmann::json::array({nlohmann::json{
                         {"name", tool_name},
                         {"description", "Echo"},
                         {"inputSchema", nlohmann::json{{"type", "object"}}}}})}};
                if (more) result["nextCursor"] = "page-" + std::to_string(page + 1);
                Reply(res, body, result);
                return;
            }
            if (method == "tools/call") {
                if (options_.call_delay_ms > 0) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(options_.call_delay_ms));
                }
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
        if (options_.id_as_string) {
            message["id"] = std::to_string(body.at("id").get<long long>());
        }
        if (options_.no_response_frame && body.value("method", "") != "initialize") {
            // SSE frames that are protocol traffic but never OUR response:
            // a server notification, no id at all. Initialize still answers
            // normally -- the handshake cannot opt out, or Open would fail.
            res.set_content(
                "event: message\n"
                "data: {\"jsonrpc\":\"2.0\",\"method\":\"notifications/progress\"}\n\n",
                "text/event-stream");
            return;
        }
        if (options_.reply_sse) {
            res.set_content("event: message\ndata: " + message.dump() + "\n\n",
                            "text/event-stream");
        } else {
            res.set_content(message.dump(), "application/json");
        }
    }

    void ReplyError(httplib::Response& res, const nlohmann::json& body,
                    const nlohmann::json& error) const {
        nlohmann::json message = {{"jsonrpc", "2.0"}, {"id", body.at("id")}, {"error", error}};
        if (options_.reply_sse) {
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
    int list_pages_served_ = 0;
    std::string session_id_;
    int session_counter_ = 0;
    Options options_;
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
