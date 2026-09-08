// tools-windbg: the tool pair against a FAKE dbgd on loopback -- the real
// HTTP layer, a hand-rolled double of dbgd's routes (POST /v1/sessions,
// POST exec with the ack-within-grace -> job pattern, GET jobs long-poll,
// POST interrupt, DELETE sessions). Exactly the layer a fake transport
// would paper over; the real dbgd's semantics this exercises are the ones
// the client actually depends on.

#include <catch2/catch_test_macros.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "dbgd_client.hpp"
#include "windbg_settings.hpp"
#include "windbg_tools.hpp"
#include "windbg_session.hpp"

using namespace minimal;
using namespace minimal::tools_windbg;
using namespace std::chrono_literals;

namespace {

// Polls a predicate with a deadline (detached watcher effects).
template <typename Pred>
bool WaitFor(Pred&& pred, std::chrono::milliseconds timeout = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return pred();
}

// A fake dbgd: one route table, one session slot, one job slot. Captures
// the requests the client makes so tests can assert on the wire behavior.
class FakeDbgd {
public:
    FakeDbgd() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(listen_fd_ >= 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        REQUIRE(::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        REQUIRE(::listen(listen_fd_, 8) == 0);
        socklen_t length = sizeof(address);
        REQUIRE(::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&address), &length) == 0);
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this] { Serve(); });
    }

    ~FakeDbgd() {
        running_.store(false);
        // Wake the accept loop with a throwaway connection.
        int wake = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port_);
        if (wake >= 0) {
            (void)::connect(wake, reinterpret_cast<sockaddr*>(&address), sizeof(address));
            ::close(wake);
        }
        if (listen_fd_ >= 0) ::close(listen_fd_);
        if (thread_.joinable()) thread_.join();
    }

    std::string base_url() const { return "http://127.0.0.1:" + std::to_string(port_); }

    // --- scripted responses / captured requests -------------------------
    std::atomic<bool> running_{true};
    std::string connect_response = R"({"handle": "test-windbg-ab12", "protocol_version": 3})";
    // Exec answers synchronously with this body unless job_mode is on.
    bool job_mode = false;
    std::string exec_sync_response = R"({"ok": true, "output": "fast output",
        "state": {"state": "stopped"}, "protocol_version": 3})";
    std::string exec_ack_response = R"({"ok": true, "running": true,
        "output": "", "job_id": "j1", "protocol_version": 3})";
    // Job reads: each entry is served once, in order; the LAST one repeats.
    std::vector<std::string> job_responses = {R"({"job_id": "j1", "output": "chunk one\n",
        "next_offset": 11, "done": false, "ok": true, "protocol_version": 3})",
        R"({"job_id": "j1", "output": "chunk two\n[exec completed ok]",
        "next_offset": 40, "done": true, "ok": true, "protocol_version": 3})"};
    std::string interrupt_response = R"({"state": {"state": "stopped"}, "protocol_version": 3})";
    bool session_deleted = false;
    std::string last_delete_body;
    std::atomic<int> request_count{0};

private:
    static void SendAll(int fd, const std::string& bytes) {
        std::size_t sent = 0;
        while (sent < bytes.size()) {
            const ssize_t n = ::write(fd, bytes.data() + sent, bytes.size() - sent);
            if (n <= 0) return;
            sent += static_cast<std::size_t>(n);
        }
    }

    void Respond(int fd, const std::string& method, const std::string& body) {
        request_count.fetch_add(1);
        std::string payload;
        int status = 200;
        if (method == "POST" && body.find("/v1/sessions") == 0 &&
            body.find("/exec") == std::string::npos) {
            payload = connect_response;
        } else if (method == "POST" && body.find("/exec") != std::string::npos) {
            payload = job_mode ? exec_ack_response : exec_sync_response;
        } else if (method == "GET" && body.find("/v1/jobs/") == 0) {
            const auto index = job_index_.fetch_add(1);
            payload = job_responses[std::min<std::size_t>(
                static_cast<std::size_t>(index), job_responses.size() - 1)];
            if (static_cast<std::size_t>(index) >= job_responses.size() - 1) {
                // Repeating the last (done) read: park briefly so a
                // background watcher's long-poll does not spin the test.
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        } else if (method == "POST" && body.find("/interrupt") != std::string::npos) {
            payload = interrupt_response;
        } else if (method == "DELETE" && body.find("/v1/sessions/") == 0) {
            session_deleted = true;
            payload = R"({"closed": true, "protocol_version": 3})";
        } else {
            status = 404;
            payload = R"({"error": {"code": "usage", "message": "unknown path"}})";
        }
        std::string response = "HTTP/1.1 " + std::to_string(status) + " OK\r\n";
        response += "Content-Type: application/json\r\n";
        response += "Content-Length: " + std::to_string(payload.size()) + "\r\n";
        response += "Connection: close\r\n\r\n";
        response += payload;
        SendAll(fd, response);
    }

    void Serve() {
        while (running_.load()) {
            const int fd = ::accept(listen_fd_, nullptr, nullptr);
            if (fd < 0) continue;
            std::string request;
            char buffer[4096];
            std::size_t content_length = 0;
            std::size_t header_end = 0;
            while (true) {
                const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
                if (n <= 0) break;
                request.append(buffer, static_cast<std::size_t>(n));
                if (header_end == 0) {
                    const auto end = request.find("\r\n\r\n");
                    if (end != std::string::npos) {
                        header_end = end + 4;
                        const auto cl = request.find("Content-Length: ");
                        if (cl != std::string::npos && cl < header_end) {
                            content_length =
                                std::stoul(request.substr(cl + 16,
                                                          request.find("\r\n", cl) - cl - 16));
                        }
                    }
                }
                if (header_end > 0 && request.size() >= header_end + content_length) break;
            }
            const auto line_end = request.find("\r\n");
            std::string request_line = request.substr(0, line_end);
            const auto sp1 = request_line.find(' ');
            const auto sp2 = request_line.find(' ', sp1 + 1);
            last_delete_body = request_line;
            // Record method + path for the route match (body parsing is
            // deliberately shallow: the client's request shapes are
            // exercised through the settings the tests control).
            std::string path = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
            const auto query = path.find('?');
            if (query != std::string::npos) path = path.substr(0, query);
            Respond(fd, request_line.substr(0, sp1), path);
            ::close(fd);
        }
    }

    int listen_fd_ = -1;
    int port_ = 0;
    std::atomic<int> job_index_{0};
    std::thread thread_;
};

WindbgSettings SettingsFor(const FakeDbgd& server) {
    WindbgSettings settings;
    settings.base_url = server.base_url();
    settings.exec_grace_s = 0;  // never wait out a grace in tests
    settings.request_timeout_ms = 5000;
    settings.job_wait_s = 0;
    return settings;
}

tools::ToolContext Context() {
    tools::ToolContext ctx{common::DefaultLogger()};
    ctx.session_id = "s1";
    ctx.project_root = std::filesystem::temp_directory_path();
    return ctx;
}

}  // namespace

TEST_CASE("dbgd client: connect, sync exec, interrupt, delete", "[windbg][client]") {
    FakeDbgd server;
    DbgdClient client(SettingsFor(server));

    DbgdError error;
    const auto handle = client.Connect(nlohmann::json{{"kind", "pid"}, {"value", 4242}}, &error);
    REQUIRE(error.code.empty());
    CHECK(handle == "test-windbg-ab12");

    const auto exec = client.Exec(handle, "lm", false, &error);
    REQUIRE(error.code.empty());
    CHECK(exec.ok);
    CHECK(exec.job_id.empty());
    CHECK(exec.output == "fast output");

    const auto interrupt = client.Interrupt(handle, &error);
    REQUIRE(error.code.empty());

    client.Close(handle, "detach");
    CHECK(server.session_deleted);
}

TEST_CASE("dbgd client: protocol version mismatch is refused before the payload",
          "[windbg][client]") {
    FakeDbgd server;
    server.exec_sync_response = R"({"ok": true, "protocol_version": 99})";
    DbgdClient client(SettingsFor(server));
    DbgdError error;
    const auto handle = client.Connect(nlohmann::json{{"kind", "pid"}}, &error);
    REQUIRE(error.code.empty());
    const auto exec = client.Exec(handle, "lm", false, &error);
    CHECK(exec.ok);  // default-constructed result...
    CHECK(exec.output.empty());  // ...nothing interpreted
    CHECK(error.code == "version_mismatch");
}

TEST_CASE("dbgd client: error bodies map to the code taxonomy", "[windbg][client]") {
    FakeDbgd server;
    // Unknown path -> 404 with dbgd's error shape.
    DbgdClient client(SettingsFor(server));
    DbgdError error;
    client.State("nope-windbg-ab12", &error);
    CHECK(error.code == "usage");
    CHECK(error.status == 404);
}

TEST_CASE("windbg tool: sync exec returns the transcript", "[windbg][tool]") {
    FakeDbgd server;
    WindbgSettings settings = SettingsFor(server);
    DbgdClient client(settings);
    WindbgSessionStore store(client, "detach");
    sidecar::TaskRegistry jobs;

    auto tool = MakeWindbgTool(settings, client, store, jobs);
    auto ctx = Context();
    const auto result = tool->execute(
        nlohmann::json{{"command", "lm"}, {"target", {{"kind", "pid"}, {"value", 4242}}}}, ctx);
    CHECK_FALSE(result.is_error);
    CHECK(result.content_text.find("fast output") != std::string::npos);
}

TEST_CASE("windbg tool: async exec starts a background task that injects the result",
          "[windbg][tool]") {
    FakeDbgd server;
    server.job_mode = true;
    WindbgSettings settings = SettingsFor(server);
    DbgdClient client(settings);
    WindbgSessionStore store(client, "detach");
    sidecar::TaskRegistry jobs;

    auto tool = MakeWindbgTool(settings, client, store, jobs);
    auto ctx = Context();

    std::mutex mu;
    std::optional<tools::ToolResult> injected;
    ctx.schedule_injection = [&](llm::Message message) {
        std::lock_guard<std::mutex> lock(mu);
        for (const auto& block : message.content) {
            if (const auto* result = std::get_if<llm::ToolResultBlock>(&block)) {
                std::string text;
                for (const auto& part : result->content) {
                    text += std::get<llm::TextBlock>(part).text;
                }
                injected = tools::ToolResult{result->is_error, text};
            }
        }
        return true;
    };
    // The session is alive for the duration of the test: the watcher polls
    // THIS token, and a dead one would (correctly) end the job as kKilled.
    const auto lifetime = std::make_shared<const int>(1);
    ctx.session_lifetime = std::weak_ptr<const void>(lifetime);

    const auto result = tool->execute(
        nlohmann::json{{"command", "g"}, {"target", {{"kind", "exec"}, {"value", "app.exe"}}}},
        ctx);
    CHECK_FALSE(result.is_error);
    CHECK(result.content_text.find("task wdbg-") != std::string::npos);

    // The watcher long-polls the job and injects the transcript at a turn
    // boundary.
    CHECK(WaitFor([&] {
        std::lock_guard<std::mutex> lock(mu);
        return injected.has_value();
    }));
    std::lock_guard<std::mutex> lock(mu);
    REQUIRE(injected.has_value());
    CHECK_FALSE(injected->is_error);
    CHECK(injected->content_text.find("chunk one") != std::string::npos);
    CHECK(injected->content_text.find("[exec completed ok]") != std::string::npos);

    store.Erase("s1", "test teardown");
}

TEST_CASE("windbg tool: session ends close the remote session", "[windbg][tool]") {
    FakeDbgd server;
    WindbgSettings settings = SettingsFor(server);
    DbgdClient client(settings);
    WindbgSessionStore store(client, "kill");
    sidecar::TaskRegistry jobs;

    auto tool = MakeWindbgTool(settings, client, store, jobs);
    auto ctx = Context();
    const auto result =
        tool->execute(nlohmann::json{{"command", "lm"},
                                     {"target", {{"kind", "pid"}, {"value", 7}}}},
                      ctx);
    CHECK_FALSE(result.is_error);

    store.Erase("s1", "session ended");
    CHECK(server.session_deleted);
}
