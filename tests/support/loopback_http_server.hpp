#pragma once

// One-shot loopback HTTP server for tests that exercise a real HTTP client:
// one canned response per accepted connection, in order. Each request's head
// (request line + headers) is recorded, so a test can check what was sent --
// the URL a paginating client built, the auth header it carried.

#include <catch2/catch_test_macros.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace minimal::test {

class LoopbackHttpServer {
public:
    explicit LoopbackHttpServer(std::vector<std::string> responses)
        : responses_(std::move(responses)) {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(listen_fd_ >= 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        REQUIRE(::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        REQUIRE(::listen(listen_fd_, 1) == 0);
        socklen_t length = sizeof(address);
        REQUIRE(::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&address), &length) == 0);
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this] { Serve(); });
    }

    ~LoopbackHttpServer() {
        if (listen_fd_ >= 0) ::close(listen_fd_);
        if (thread_.joinable()) thread_.join();
    }

    LoopbackHttpServer(const LoopbackHttpServer&) = delete;
    LoopbackHttpServer& operator=(const LoopbackHttpServer&) = delete;

    std::string url(std::string_view path = "/") const {
        return "http://127.0.0.1:" + std::to_string(port_) + std::string(path);
    }

    // The heads of the requests received so far, in arrival order.
    std::vector<std::string> requests() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }

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
            if (n <= 0) return;
            sent += static_cast<std::size_t>(n);
        }
    }

    void Serve() {
        timeval tv{5, 0};
        (void)::setsockopt(listen_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        for (const auto& response : responses_) {
            int fd = ::accept(listen_fd_, nullptr, nullptr);
            if (fd < 0) return;
            std::string request;
            char buffer[4096];
            while (request.find("\r\n\r\n") == std::string::npos) {
                ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
                if (n <= 0) break;
                request.append(buffer, static_cast<std::size_t>(n));
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                requests_.push_back(request.substr(0, request.find("\r\n\r\n")));
            }
            SendAll(fd, response);
            ::close(fd);
        }
    }

    int listen_fd_ = -1;
    int port_ = 0;
    std::vector<std::string> responses_;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::vector<std::string> requests_;
};

inline std::string HttpResponseOf(int status, std::string_view status_text,
                                  std::string_view content_type, std::string_view body,
                                  std::vector<std::string> extra_headers = {}) {
    std::string out = "HTTP/1.1 " + std::to_string(status) + " " + std::string(status_text) + "\r\n";
    if (!content_type.empty()) out += "Content-Type: " + std::string(content_type) + "\r\n";
    out += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    for (const auto& header : extra_headers) out += header + "\r\n";
    out += "Connection: close\r\n\r\n";
    out += std::string(body);
    return out;
}

}  // namespace minimal::test
