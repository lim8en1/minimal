// tools-web: URL policy (SSRF guard), HTML->text extraction, and the two
// tools (web_fetch, web_search). The tool tests run against a real loopback
// HTTP server (same pattern as http_client_test.cpp's LoopbackServer) --
// exactly the layer a fake would paper over.

#include <catch2/catch_test_macros.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <thread>

#include "fetch_tool.hpp"
#include "html_text.hpp"
#include "search_tool.hpp"
#include "url_policy.hpp"
#include "web_settings.hpp"

using namespace minimal;
using namespace minimal::tools_web;

namespace {

// One-shot loopback HTTP server, one response per accepted connection --
// mirrors http_client_test.cpp's LoopbackServer.
class LoopbackServer {
public:
    explicit LoopbackServer(std::vector<std::string> responses) : responses_(std::move(responses)) {
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

    ~LoopbackServer() {
        if (listen_fd_ >= 0) ::close(listen_fd_);
        if (thread_.joinable()) thread_.join();
    }

    std::string url(std::string_view path = "/") const {
        return "http://127.0.0.1:" + std::to_string(port_) + std::string(path);
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
            SendAll(fd, response);
            ::close(fd);
        }
    }

    int listen_fd_ = -1;
    int port_ = 0;
    std::vector<std::string> responses_;
    std::thread thread_;
};

std::string HttpResponseOf(int status, std::string_view status_text, std::string_view content_type,
                           std::string_view body, std::vector<std::string> extra_headers = {}) {
    std::string out = "HTTP/1.1 " + std::to_string(status) + " " + std::string(status_text) + "\r\n";
    if (!content_type.empty()) out += "Content-Type: " + std::string(content_type) + "\r\n";
    out += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    for (const auto& header : extra_headers) out += header + "\r\n";
    out += "Connection: close\r\n\r\n";
    out += std::string(body);
    return out;
}

tools::ToolContext Context() {
    tools::ToolContext ctx{common::DefaultLogger()};
    ctx.session_id = "s1";
    ctx.project_root = std::filesystem::temp_directory_path();
    return ctx;
}

// LoopbackServer only ever binds 127.0.0.1, which url_policy refuses by
// default (that refusal is exactly what the dedicated "loopback URL is
// refused" test below checks) -- every OTHER fetch test exercises the
// transport/parsing behavior against that same loopback server, so it opts
// back in explicitly.
WebSettings LocalSettings() {
    WebSettings settings;
    settings.fetch_allow_private_hosts = true;
    return settings;
}

}  // namespace

// ---------------------------------------------------------------------------
// url_policy
// ---------------------------------------------------------------------------

TEST_CASE("CheckUrl: only http/https schemes are accepted", "[tools_web][url_policy]") {
    CHECK_FALSE(CheckUrl("ftp://example.com/", true).ok);
    CHECK_FALSE(CheckUrl("file:///etc/passwd", true).ok);
    CHECK(CheckUrl("http://93.184.216.34/", true).ok);
}

TEST_CASE("CheckUrl: loopback and private literal addresses are refused by default",
         "[tools_web][url_policy]") {
    CHECK_FALSE(CheckUrl("http://127.0.0.1/", false).ok);
    CHECK_FALSE(CheckUrl("http://localhost/", false).ok);
    CHECK_FALSE(CheckUrl("http://10.0.0.5/", false).ok);
    CHECK_FALSE(CheckUrl("http://192.168.1.1/", false).ok);
    CHECK_FALSE(CheckUrl("http://172.16.0.1/", false).ok);
    // The single most common real-world SSRF target: cloud instance metadata.
    CHECK_FALSE(CheckUrl("http://169.254.169.254/", false).ok);
    CHECK_FALSE(CheckUrl("http://[::1]/", false).ok);
}

TEST_CASE("CheckUrl: a public literal address is allowed by default",
         "[tools_web][url_policy]") {
    // A literal IP needs no DNS resolution (getaddrinfo parses it directly),
    // so this is hermetic -- no network access happens in this test.
    const auto check = CheckUrl("https://93.184.216.34/", false);
    CHECK(check.ok);
    CHECK(check.host == "93.184.216.34");
}

TEST_CASE("CheckUrl: allow_private_hosts opts out of resolution entirely",
         "[tools_web][url_policy]") {
    CHECK(CheckUrl("http://127.0.0.1/", true).ok);
    CHECK(CheckUrl("http://169.254.169.254/", true).ok);
}

TEST_CASE("CheckUrl: an unparseable URL is refused with a reason", "[tools_web][url_policy]") {
    const auto check = CheckUrl("not-a-url-at-all", false);
    CHECK_FALSE(check.ok);
    CHECK_FALSE(check.reason.empty());
}

// ---------------------------------------------------------------------------
// html_text
// ---------------------------------------------------------------------------

TEST_CASE("HtmlToText: strips tags and decodes common entities", "[tools_web][html_text]") {
    const auto text = HtmlToText("<p>Tom &amp; Jerry say &quot;hi&quot;</p>");
    CHECK(text.find("Tom & Jerry say \"hi\"") != std::string::npos);
    CHECK(text.find('<') == std::string::npos);
}

TEST_CASE("HtmlToText: script and style content is dropped wholesale",
         "[tools_web][html_text]") {
    const auto text = HtmlToText(
        "<html><head><style>body{color:red}</style></head>"
        "<body><script>alert('x')</script><p>real content</p></body></html>");
    CHECK(text.find("real content") != std::string::npos);
    CHECK(text.find("color") == std::string::npos);
    CHECK(text.find("alert") == std::string::npos);
}

TEST_CASE("HtmlToText: block elements become line breaks", "[tools_web][html_text]") {
    const auto text = HtmlToText("<p>one</p><p>two</p>");
    const auto first_line_end = text.find('\n');
    REQUIRE(first_line_end != std::string::npos);
    CHECK(text.substr(0, first_line_end).find("one") != std::string::npos);
    CHECK(text.find("two") > first_line_end);
}

TEST_CASE("HtmlToText: numeric entities decode to UTF-8", "[tools_web][html_text]") {
    const auto text = HtmlToText("caf&#233; and &#x2014;dash");   // e-acute, em dash
    CHECK(text.find("caf\xC3\xA9") != std::string::npos);
    CHECK(text.find("\xE2\x80\x94""dash") != std::string::npos);
}

TEST_CASE("HtmlToText: malformed markup does not hang or crash", "[tools_web][html_text]") {
    CHECK_NOTHROW(HtmlToText("<div><p>unterminated"));
    CHECK_NOTHROW(HtmlToText("<script>never closed"));
}

// ---------------------------------------------------------------------------
// web_fetch
// ---------------------------------------------------------------------------

TEST_CASE("web_fetch: plain text response is returned as-is", "[tools_web][fetch]") {
    LoopbackServer server({HttpResponseOf(200, "OK", "text/plain", "hello world")});
    auto tool = MakeWebFetchTool(LocalSettings(), net::HttpClient{});
    auto ctx = Context();
    const auto result = tool->execute({{"url", server.url()}}, ctx);
    CHECK_FALSE(result.is_error);
    CHECK(result.content_text == "hello world");
}

TEST_CASE("web_fetch: an HTML response is converted to text", "[tools_web][fetch]") {
    LoopbackServer server({HttpResponseOf(200, "OK", "text/html", "<p>hello <b>world</b></p>")});
    auto tool = MakeWebFetchTool(LocalSettings(), net::HttpClient{});
    auto ctx = Context();
    const auto result = tool->execute({{"url", server.url()}}, ctx);
    CHECK_FALSE(result.is_error);
    CHECK(result.content_text.find("hello world") != std::string::npos);
    CHECK(result.content_text.find('<') == std::string::npos);
}

TEST_CASE("web_fetch: a non-2xx status is a tool error", "[tools_web][fetch]") {
    LoopbackServer server({HttpResponseOf(404, "Not Found", "text/plain", "gone")});
    auto tool = MakeWebFetchTool(LocalSettings(), net::HttpClient{});
    auto ctx = Context();
    const auto result = tool->execute({{"url", server.url()}}, ctx);
    CHECK(result.is_error);
    CHECK(result.content_text.find("404") != std::string::npos);
}

TEST_CASE("web_fetch: an absolute redirect is followed", "[tools_web][fetch]") {
    LoopbackServer target({HttpResponseOf(200, "OK", "text/plain", "final")});
    const std::string target_url = target.url();
    LoopbackServer redirector(
        {HttpResponseOf(302, "Found", "text/plain", "", {"Location: " + target_url})});

    auto tool = MakeWebFetchTool(LocalSettings(), net::HttpClient{});
    auto ctx = Context();
    const auto result = tool->execute({{"url", redirector.url()}}, ctx);
    CHECK_FALSE(result.is_error);
    CHECK(result.content_text == "final");
}

TEST_CASE("web_fetch: a relative redirect is refused, not guessed at", "[tools_web][fetch]") {
    LoopbackServer server(
        {HttpResponseOf(302, "Found", "text/plain", "", {"Location: /login"})});
    auto tool = MakeWebFetchTool(LocalSettings(), net::HttpClient{});
    auto ctx = Context();
    const auto result = tool->execute({{"url", server.url()}}, ctx);
    CHECK(result.is_error);
    CHECK(result.content_text.find("absolute") != std::string::npos);
}

TEST_CASE("web_fetch: a loopback URL is refused before any request is made",
         "[tools_web][fetch]") {
    auto tool = MakeWebFetchTool(WebSettings{}, net::HttpClient{});
    auto ctx = Context();
    const auto result = tool->execute({{"url", "http://127.0.0.1:1/"}}, ctx);
    CHECK(result.is_error);
    CHECK(result.content_text.find("refused") != std::string::npos);
}

TEST_CASE("web_fetch: allow_private_hosts opts a loopback target back in",
         "[tools_web][fetch]") {
    LoopbackServer server({HttpResponseOf(200, "OK", "text/plain", "local")});
    WebSettings settings;
    settings.fetch_allow_private_hosts = true;
    auto tool = MakeWebFetchTool(settings, net::HttpClient{});
    auto ctx = Context();
    const auto result = tool->execute({{"url", server.url()}}, ctx);
    CHECK_FALSE(result.is_error);
    CHECK(result.content_text == "local");
}

TEST_CASE("web_fetch: 'url' is required", "[tools_web][fetch]") {
    auto tool = MakeWebFetchTool(WebSettings{}, net::HttpClient{});
    auto ctx = Context();
    const auto result = tool->execute(nlohmann::json::object(), ctx);
    CHECK(result.is_error);
}

TEST_CASE("web_fetch: declares network_access_required", "[tools_web][fetch]") {
    auto tool = MakeWebFetchTool(WebSettings{}, net::HttpClient{});
    CHECK(tool->network_access_required());
}

// ---------------------------------------------------------------------------
// web_search
// ---------------------------------------------------------------------------

TEST_CASE("web_search: reports plainly when unconfigured", "[tools_web][search]") {
    auto tool = MakeWebSearchTool(WebSettings{}, net::HttpClient{});
    auto ctx = Context();
    const auto result = tool->execute({{"query", "test"}}, ctx);
    CHECK(result.is_error);
    CHECK(result.content_text.find("not configured") != std::string::npos);
}

TEST_CASE("web_search: a configured backend's results are formatted", "[tools_web][search]") {
    LoopbackServer server({HttpResponseOf(
        200, "OK", "application/json",
        R"({"results":[{"title":"Example","url":"https://example.com","snippet":"a page"}]})")});

    WebSettings settings;
    settings.search_api_url = server.url();
    auto tool = MakeWebSearchTool(settings, net::HttpClient{});
    auto ctx = Context();
    const auto result = tool->execute({{"query", "test"}}, ctx);
    CHECK_FALSE(result.is_error);
    CHECK(result.content_text.find("Example") != std::string::npos);
    CHECK(result.content_text.find("https://example.com") != std::string::npos);
    CHECK(result.content_text.find("a page") != std::string::npos);
}

TEST_CASE("web_search: an unexpected backend shape is a clear tool error",
         "[tools_web][search]") {
    LoopbackServer server(
        {HttpResponseOf(200, "OK", "application/json", R"({"unexpected": true})")});
    WebSettings settings;
    settings.search_api_url = server.url();
    auto tool = MakeWebSearchTool(settings, net::HttpClient{});
    auto ctx = Context();
    const auto result = tool->execute({{"query", "test"}}, ctx);
    CHECK(result.is_error);
    CHECK(result.content_text.find("results") != std::string::npos);
}

TEST_CASE("web_search: 'query' is required", "[tools_web][search]") {
    WebSettings settings;
    settings.search_api_url = "http://127.0.0.1:1/";
    auto tool = MakeWebSearchTool(settings, net::HttpClient{});
    auto ctx = Context();
    const auto result = tool->execute(nlohmann::json::object(), ctx);
    CHECK(result.is_error);
}

// ---------------------------------------------------------------------------
// web_settings
// ---------------------------------------------------------------------------

TEST_CASE("ParseWebSettings: defaults with an empty config", "[tools_web][settings]") {
    const auto settings = ParseWebSettings(nlohmann::json::object());
    CHECK(settings.fetch_timeout_ms == 15000);
    CHECK(settings.fetch_allow_private_hosts == false);
    CHECK(settings.search_api_url.empty());
}

TEST_CASE("ParseWebSettings: reads fetch.* and search.*", "[tools_web][settings]") {
    const nlohmann::json config = {
        {"fetch", {{"timeout_ms", 5000}, {"max_redirects", 2}, {"allow_private_hosts", true}}},
        {"search", {{"api_url", "http://example.com/search"}, {"default_max_results", 3}}}};
    const auto settings = ParseWebSettings(config);
    CHECK(settings.fetch_timeout_ms == 5000);
    CHECK(settings.fetch_max_redirects == 2);
    CHECK(settings.fetch_allow_private_hosts == true);
    CHECK(settings.search_api_url == "http://example.com/search");
    CHECK(settings.search_default_max_results == 3);
}

TEST_CASE("ParseWebSettings: an unset search.api_key_env throws", "[tools_web][settings]") {
    const nlohmann::json config = {{"search", {{"api_key_env", "MINIMAL_TEST_UNSET_VAR_XYZ"}}}};
    CHECK_THROWS_AS(ParseWebSettings(config), std::runtime_error);
}
