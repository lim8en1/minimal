#include <catch2/catch_test_macros.hpp>

#include "minimal/llm/http_client.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

using namespace minimal::llm;

namespace {

// One-shot loopback HTTP server for the transport tests: binds an ephemeral
// port on 127.0.0.1, serves `responses` (one per accepted connection, in
// order) from a background thread, and stops when the listen socket closes.
class LoopbackServer {
public:
    explicit LoopbackServer(std::vector<std::string> responses) : responses_(std::move(responses)) {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(listen_fd_ >= 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;  // ephemeral
        REQUIRE(::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        REQUIRE(::listen(listen_fd_, 1) == 0);
        socklen_t length = sizeof(address);
        REQUIRE(::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&address), &length) == 0);
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this] { Serve(); });
    }

    ~LoopbackServer() {
        if (listen_fd_ >= 0) ::close(listen_fd_);  // wakes the accept loop
        if (thread_.joinable()) thread_.join();
    }

    int port() const { return port_; }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/v1/models"; }

private:
    static void SendAll(int fd, const std::string& bytes) {
#ifdef SO_NOSIGPIPE
        int one = 1;
        (void)::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        std::size_t sent = 0;
        while (sent < bytes.size()) {
#ifdef MSG_NOSIGNAL
            ssize_t n = ::send(fd, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
#else
            ssize_t n = ::write(fd, bytes.data() + sent, bytes.size() - sent);
#endif
            if (n <= 0) return;  // client went away (e.g. body-cap abort) -- stop serving
            sent += static_cast<std::size_t>(n);
        }
    }

    void Serve() {
        // Bound the accept wait so a test that never connects cannot hang the
        // suite: the thread exits after the timeout with the socket unbound.
        timeval tv{5, 0};
        (void)::setsockopt(listen_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        for (const auto& response : responses_) {
            int fd = ::accept(listen_fd_, nullptr, nullptr);
            if (fd < 0) return;  // listen closed or timed out
            // Drain the request head (method/headers) before answering; the
            // tests make no assertions about it, it just must not confuse the
            // response framing.
            std::string request;
            char buffer[4096];
            while (request.find("\r\n\r\n") == std::string::npos) {
                ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
                if (n <= 0) break;
                request.append(buffer, static_cast<std::size_t>(n));
            }
            SendAll(fd, response);
            ::close(fd);
        }
    }

    int listen_fd_ = -1;
    int port_ = 0;
    std::vector<std::string> responses_;
    std::thread thread_;
};

std::string SimpleResponse(std::string_view body) {
    return "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
           std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + std::string(body);
}

}  // namespace

TEST_CASE("FindHeaderCI matches regardless of case", "[http_client]") {
    std::vector<HttpHeader> headers{{"Content-Type", "application/json"}, {"Retry-After", "30"}};

    CHECK(FindHeaderCI(headers, "content-type") == "application/json");
    CHECK(FindHeaderCI(headers, "CONTENT-TYPE") == "application/json");
    CHECK(FindHeaderCI(headers, "retry-after") == "30");
}

TEST_CASE("FindHeaderCI returns nullopt when absent", "[http_client]") {
    std::vector<HttpHeader> headers{{"Content-Type", "application/json"}};
    CHECK_FALSE(FindHeaderCI(headers, "Authorization").has_value());
    CHECK_FALSE(FindHeaderCI({}, "Content-Type").has_value());
}

TEST_CASE("Get exercises the request path and classifies connect failures", "[http_client]") {
    HttpClient http;
    HttpRequest request;
    request.url = "http://127.0.0.1:1/models";  // port 1: nothing listens
    request.connect_timeout = std::chrono::milliseconds(500);

    try {
        (void)http.Get(request);
        FAIL("expected a transport error");
    } catch (const HttpError& error) {
        CHECK(error.kind() == HttpErrorKind::ConnectFailed);
    }
}

TEST_CASE("Get round-trips status, headers and body over a real loopback connection",
          "[http_client]") {
    // This is the test that pins the GET transport: Mode::Get originally fell
    // into the streaming branch of WriteCallback and silently dropped every
    // response body, which the canned-body catalog tests could never see.
    LoopbackServer server({SimpleResponse(R"({"data":[{"id":"local-model"}]})")});
    HttpClient http;
    HttpRequest request;
    request.url = server.url();
    request.connect_timeout = std::chrono::milliseconds(2000);
    request.request_timeout = std::chrono::milliseconds(2000);

    HttpResponse response = http.Get(request);
    CHECK(response.status == 200);
    CHECK(response.body == R"({"data":[{"id":"local-model"}]})");
    CHECK(FindHeaderCI(response.headers, "Content-Type") == "application/json");
}

TEST_CASE("Post buffers the body over a real loopback connection", "[http_client]") {
    LoopbackServer server({SimpleResponse(R"({"ok":true})")});
    HttpClient http;
    HttpRequest request;
    request.url = server.url();
    request.body = R"({"messages":[]})";
    request.connect_timeout = std::chrono::milliseconds(2000);
    request.request_timeout = std::chrono::milliseconds(2000);

    HttpResponse response = http.Post(request);
    CHECK(response.status == 200);
    CHECK(response.body == R"({"ok":true})");
}

TEST_CASE("a buffered response past the size cap aborts with an error", "[http_client]") {
    // 8 MiB cap + 1 byte; the server's write fails mid-body when the client
    // aborts, which SendAll handles by stopping.
    std::string body(kMaxResponseBodyBytes + 1, 'x');
    LoopbackServer server({SimpleResponse(body)});
    HttpClient http;
    HttpRequest request;
    request.url = server.url();
    request.connect_timeout = std::chrono::milliseconds(2000);
    request.request_timeout = std::chrono::milliseconds(10000);

    try {
        (void)http.Get(request);
        FAIL("expected the body cap to abort the transfer");
    } catch (const HttpError& error) {
        CHECK(error.kind() == HttpErrorKind::Other);
        CHECK(std::string(error.what()).find("response body exceeds limit") != std::string::npos);
    }
}

namespace {

// A loopback server that consumes the request body SLOWLY but STEADILY -- the
// wire shape of a modest uplink. It reads 32 KiB per 30 ms slice, so a 2 MiB
// upload takes ~2 s of continuously-advancing upload progress with zero
// response bytes until the end.
class SlowReadServer {
public:
    SlowReadServer() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(listen_fd_ >= 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        REQUIRE(::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        REQUIRE(::listen(listen_fd_, 1) == 0);
        // On loopback the kernel buffers absorb a small body instantly, which
        // would make curl finish "uploading" before the drain even starts --
        // and the post-upload silence would then be a LEGITIMATE idle abort. A
        // tiny receive window keeps curl actively writing for the whole drain,
        // which is what "slow uplink" actually looks like.
        int rcvbuf = 16 * 1024;
        (void)::setsockopt(listen_fd_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
        socklen_t length = sizeof(address);
        REQUIRE(::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&address), &length) == 0);
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this] { Serve(); });
    }
    ~SlowReadServer() {
        if (listen_fd_ >= 0) ::close(listen_fd_);
        if (thread_.joinable()) thread_.join();
    }
    int port() const { return port_; }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/v1/chat"; }

private:
    void Serve() {
        timeval tv{10, 0};
        (void)::setsockopt(listen_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        int fd = ::accept(listen_fd_, nullptr, nullptr);
        if (fd < 0) return;
        std::string request;
        char buffer[32 * 1024];
        // Headers first; Content-Length is how the server knows when the body
        // is done (the client then waits for the response, so a server that
        // kept recv-ing would deadlock the shape this test exists to prove).
        std::size_t content_length = 0;
        while (request.find("\r\n\r\n") == std::string::npos) {
            ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
            if (n <= 0) {
                ::close(fd);
                return;
            }
            request.append(buffer, static_cast<std::size_t>(n));
        }
        if (const auto marker = request.find("Content-Length: "); marker != std::string::npos) {
            content_length = std::strtoull(request.c_str() + marker + 15, nullptr, 10);
        }
        const auto body_start = request.find("\r\n\r\n") + 4;
        std::size_t received = request.size() - body_start;
        // The body, in slices -- each slice is upload progress the transfer
        // callback must count, even though dlnow stays 0 the whole time.
        while (received < content_length) {
            ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
            if (n <= 0) break;
            received += static_cast<std::size_t>(n);
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        const std::string response = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
        (void)!::send(fd, response.data(), response.size(), 0);
        ::close(fd);
    }

    int listen_fd_ = -1;
    int port_ = 0;
    std::thread thread_;
};

}  // namespace

TEST_CASE("upload progress counts toward the streaming idle timeout", "[http_client][invariant]") {
    // The idle callback used to watch DOWNLOADS only. During the entire upload
    // phase of a streaming POST, dlnow == 0 -- so a multi-MB serialized request
    // (the 512k-1M-token shape) on a modest uplink was killed at the idle
    // timeout as "stalled", classified retryable, and failed identically until
    // the retries ran out. The server here drains the upload steadily for ~4 s
    // against a 2 s idle timeout: comfortably under the drain for the OLD code
    // (which aborts at its first callback past the timeout -- upload progress
    // never moved anything), comfortably above any scheduler stall a loaded
    // run inflicts on the new one.
    SlowReadServer server;
    HttpClient http;
    HttpRequest request;
    request.url = server.url();
    request.body = std::string(2ull << 20, 'x');  // 2 MiB: ~64 read slices
    request.connect_timeout = std::chrono::milliseconds(2000);
    request.request_timeout = std::chrono::milliseconds(0);  // streaming shape: idle-only
    request.idle_timeout = std::chrono::milliseconds(2000);

    // Streaming mode hands the body to the sink -- response.body stays empty.
    std::string body;
    HttpResponse response;
    std::string failure;
    try {
        response = http.StreamPost(request, [](int, const std::vector<HttpHeader>&) {},
                                   [&body](std::string_view chunk) {
                                       body.append(chunk);
                                       return true;
                                   });
    } catch (const HttpError& error) {
        failure = error.what();
    }
    INFO("transfer failed: " << failure);
    CHECK(failure.empty());
    CHECK(response.status == 200);
    CHECK(body == "ok");
}
