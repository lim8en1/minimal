// One in-process streamable-http MCP server, scripted per test. Shared by the
// connection and manager tests. Deterministic: binds an ephemeral localhost
// port. httplib comes from cpp-mcp's bundled copy (SYSTEM include, wired for
// the whole test target in tests/CMakeLists.txt).
#pragma once

#include <atomic>
#include <condition_variable>
#include <chrono>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>

#include <httplib.h>

class FakeHttpMcpServer {
public:
    struct Options {
        bool reply_sse = true;
        int list_pages = 1;             // >1: tools/list answers with nextCursor pages
        bool id_as_string = false;      // echoes JSON-RPC ids as strings
        bool no_response_frame = false; // SSE reply carrying only unrelated frames
        int call_delay_ms = 0;          // tools/call sleeps before answering
        bool gate_initialize = false;   // initialize blocked until ReleaseGate()
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
                AwaitGate();  // no-op unless gated
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
            if (method == "ping") {
                Reply(res, body, nlohmann::json::object());
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
        ReleaseGate();
        server_.stop();
        if (thread_.joinable()) thread_.join();
    }

    std::string url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/mcp"; }
    int requests() const { return requests_; }

    // Gate support: with gate_initialize, the initialize handler parks until
    // released -- a deterministic way to hold one server's connect while
    // proving another operation does not wait behind it.
    void ReleaseGate() {
        {
            std::lock_guard<std::mutex> lock(gate_mu_);
            gate_open_ = true;
        }
        gate_cv_.notify_all();
    }

private:
    void AwaitGate() {
        if (!options_.gate_initialize) return;
        std::unique_lock<std::mutex> lock(gate_mu_);
        gate_cv_.wait(lock, [this] { return gate_open_; });
    }

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

    std::mutex gate_mu_;
    std::condition_variable gate_cv_;
    bool gate_open_ = false;
};
