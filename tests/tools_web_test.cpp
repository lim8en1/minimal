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
#include <fstream>
#include <filesystem>
#include <limits>
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
    // CGNAT (RFC 6598) and the IETF protocol-assignment block (RFC 7335).
    CHECK_FALSE(CheckUrl("http://100.64.0.1/", false).ok);
    CHECK_FALSE(CheckUrl("http://100.127.255.254/", false).ok);
    CHECK_FALSE(CheckUrl("http://192.0.0.9/", false).ok);
    // Just outside 100.64.0.0/10: still a public unicast address.
    CHECK(CheckUrl("http://100.128.0.1/", true).ok);
}

TEST_CASE("CheckUrl: newly-added special-purpose ranges are refused by default",
         "[tools_web][url_policy]") {
    CHECK_FALSE(CheckUrl("http://198.18.0.1/", false).ok);   // RFC 2544 benchmarking
    CHECK_FALSE(CheckUrl("http://198.19.255.254/", false).ok);
    CHECK_FALSE(CheckUrl("http://224.0.0.1/", false).ok);    // multicast
    CHECK_FALSE(CheckUrl("http://240.0.0.1/", false).ok);    // reserved
    CHECK_FALSE(CheckUrl("http://255.255.255.255/", false).ok);
    CHECK_FALSE(CheckUrl("http://[ff02::1]/", false).ok);    // IPv6 multicast
    CHECK_FALSE(CheckUrl("http://[2002:c000:0204::1]/", false).ok);  // 6to4 (embeds 192.0.2.4)
    // Just outside 198.18.0.0/15: a real public address.
    CHECK(CheckUrl("http://198.20.0.1/", true).ok);
}

TEST_CASE("CheckUrl: an '@' inside the query or fragment does not rename the host",
         "[tools_web][url_policy]") {
    // Regression: the userinfo-stripping search looked only for '/' as the
    // end of the authority, so an '@' placed in the query or fragment was
    // read as "user@host", vetting the string AFTER it (a public-looking
    // name) while libcurl -- which ends the authority at '/', '?' or '#' --
    // connects to the loopback/metadata address before it. Both must now
    // name the same (refused) host.
    const auto query = CheckUrl("http://127.0.0.1:8080?@example.com/", false);
    CHECK_FALSE(query.ok);
    const auto fragment = CheckUrl("http://169.254.169.254/latest/meta-data#@example.com/", false);
    CHECK_FALSE(fragment.ok);
    // A real userinfo before the authority ends is still stripped correctly.
    const auto real_userinfo = CheckUrl("https://user:pass@93.184.216.34/", false);
    CHECK(real_userinfo.ok);
    CHECK(real_userinfo.host == "93.184.216.34");
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

TEST_CASE("CheckUrl: a public literal address is allowed WITH a pin entry",
         "[tools_web][url_policy]") {
    // The companion to the malformed-port case below: the normal path must
    // still produce a non-empty resolve_entries, or the fail-closed check
    // there would also reject every legitimate request.
    const auto check = CheckUrl("https://93.184.216.34/", false);
    REQUIRE(check.ok);
    CHECK_FALSE(check.resolve_entries.empty());
}

TEST_CASE("CheckUrl: a malformed port is refused, not allowed unpinned",
         "[tools_web][url_policy]") {
    // A garbage port cannot be turned into a CURLOPT_RESOLVE pin entry.
    // This used to fall back to an unpinned ok=true (reasoning: "libcurl
    // will reject the URL itself"), which left the DNS-rebinding window the
    // pin exists to close wide open for however long the connection attempt
    // took to fail.
    const auto check = CheckUrl("https://93.184.216.34:notaport/", false);
    CHECK_FALSE(check.ok);
    CHECK(check.resolve_entries.empty());
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

TEST_CASE("HtmlToText: surrogates, NUL, and control entities are dropped, not emitted",
          "[tools_web][html_text][invariant]") {
    // A surrogate codepoint encoded as 3 raw bytes is INVALID UTF-8 (it
    // corrupts every downstream consumer); &#0; emitted a literal NUL, which
    // truncates C-string handling; other C0 controls have no business in the
    // model's text. All dropped.
    const auto surrogate = HtmlToText("&#xD800;after");
    CHECK(surrogate.find("after") != std::string::npos);
    CHECK(surrogate.find("\xED\xA0\x80") == std::string::npos);

    const auto nul = HtmlToText("before&#0;after");
    CHECK(nul.find("before") != std::string::npos);
    CHECK(nul.find("after") != std::string::npos);
    CHECK(nul.find('\0') == std::string::npos);

    const auto esc = HtmlToText("&#27;[31mred");
    CHECK(esc.find("[31mred") != std::string::npos);
    CHECK(esc.find('\x1B') == std::string::npos);
}

TEST_CASE("HtmlToText: malformed markup does not hang or crash", "[tools_web][html_text]") {
    CHECK_NOTHROW(HtmlToText("<div><p>unterminated"));
    CHECK_NOTHROW(HtmlToText("<script>never closed"));
}

TEST_CASE("HtmlToText: a stray '<' is a literal, not the start of eaten markup",
          "[tools_web][html_text]") {
    // Regression: ANY '<' was assumed to start a tag and everything up to
    // the next unrelated '>' was silently eaten -- "if x < 5 then y > 3
    // done" lost "< 5 then y >" entirely, becoming "if x 3 done".
    const auto text = HtmlToText("if x < 5 then y > 3 done");
    CHECK(text.find("x < 5") != std::string::npos);
    CHECK(text.find("y > 3") != std::string::npos);
}

TEST_CASE("HtmlToText: a comment ends at '-->', not at the first '>' inside it",
          "[tools_web][html_text]") {
    // Regression: a comment containing a literal '>' ("<!-- note > hidden
    // -->") had its "tag" end at that inner '>', leaking the comment's tail
    // ("hidden -->") into the output as ordinary text.
    const auto text = HtmlToText("before<!-- note > hidden -->visible");
    CHECK(text.find("before") != std::string::npos);
    CHECK(text.find("visible") != std::string::npos);
    CHECK(text.find("hidden") == std::string::npos);
    CHECK(text.find("note") == std::string::npos);
}

TEST_CASE("HtmlToText: a '>' inside a quoted attribute does not end the tag",
          "[tools_web][html_text]") {
    // Regression: `<a title="a>b">` ended the "tag" at the '>' inside the
    // quoted value, leaking `b">` into the output.
    const auto text = HtmlToText("<a title=\"a>b\">link text</a>");
    CHECK(text.find("link text") != std::string::npos);
    CHECK(text.find("b\">") == std::string::npos);
}

TEST_CASE("HtmlToText: a paragraph gap keeps its one blank line",
          "[tools_web][html_text]") {
    // Regression: pending_newlines could only ever reach 1 (gating the
    // increment on line_has_content, which the SAME newline event flips
    // false before a second consecutive one is counted), so the min(...,2)
    // cap was dead and every paragraph boundary collapsed to a single "\n"
    // instead of the documented "runs of blank lines to at most one".
    const auto text = HtmlToText("<p>a</p><p>b</p>");
    CHECK(text.find("a\n\nb") != std::string::npos);
}

TEST_CASE("HtmlToText: a malformed numeric entity stays literal, not silently decoded",
          "[tools_web][html_text]") {
    // Regression: stoul with a null `pos` out-param doesn't throw on a
    // PARTIAL parse ("12abc" -> 12, silently swallowing "abc"), and an
    // unchecked static_cast<uint32_t> wrapped an out-of-range value
    // ("x100000041" = 4294967361) to 'A' instead of rejecting it.
    const auto partial = HtmlToText("&#12abc;done");
    CHECK(partial.find("abc") != std::string::npos);  // not swallowed into the entity
    CHECK(partial.find("done") != std::string::npos);

    const auto overflow = HtmlToText("&#x100000041;done");
    CHECK(overflow.find('A') == std::string::npos);  // not silently wrapped
    CHECK(overflow.find("done") != std::string::npos);
}

TEST_CASE("HtmlToText: a flood of '&' stays literal and does not decode a garbage tail",
          "[tools_web][html_text]") {
    // The ';' search used to run unbounded from every '&': a page of '&'s was
    // O(N^2). Each '&' must still be emitted literally, and a long garbage
    // tail after one '&' must not be swallowed into an entity.
    const std::string flood(100000, '&');
    CHECK(HtmlToText(flood) == flood);

    const std::string tail = "&" + std::string(64, 'x');
    CHECK(HtmlToText(tail) == tail);

    // The window boundary holds: a body longer than the 13-byte entity window
    // is not an entity even when it eventually finds a ';'.
    const std::string over_window = "&" + std::string(12, 'a') + ";";
    CHECK(HtmlToText(over_window) == over_window);
    CHECK(HtmlToText("&amp;") == "&");
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

TEST_CASE("web_fetch: a binary content-type is refused, not dumped as text",
          "[tools_web][fetch]") {
    // Regression: a binary body (image, PDF, archive, ...) returned "as
    // text" ends up as raw bytes or U+FFFD junk in the model's context;
    // this tool has no rendering or extraction for it.
    LoopbackServer server({HttpResponseOf(200, "OK", "image/png", "\x89PNG\r\n\x1a\n")});
    auto tool = MakeWebFetchTool(LocalSettings(), net::HttpClient{});
    auto ctx = Context();
    const auto result = tool->execute({{"url", server.url()}}, ctx);
    CHECK(result.is_error);
    CHECK(result.content_text.find("image/png") != std::string::npos);
}

TEST_CASE("web_fetch: 'url' is required", "[tools_web][fetch]") {
    auto tool = MakeWebFetchTool(WebSettings{}, net::HttpClient{});
    auto ctx = Context();
    const auto result = tool->execute(nlohmann::json::object(), ctx);
    CHECK(result.is_error);
}

// The spill tests get a project root of their own so the file lands somewhere
// observable and contained, not in the shared temp root Context() uses.
TEST_CASE("web_fetch: an oversized response is truncated, and the full text is spilled",
          "[tools_web][fetch]") {
    const std::string body(10000, 'x');
    LoopbackServer server({HttpResponseOf(200, "OK", "text/plain", body)});
    WebSettings settings = LocalSettings();
    settings.fetch_default_max_bytes = 1000;
    auto tool = MakeWebFetchTool(settings, net::HttpClient{});

    const auto root = std::filesystem::temp_directory_path() / "minimal-webfetch-spill-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    auto ctx = Context();
    ctx.project_root = root;

    const auto result = tool->execute({{"url", server.url()}}, ctx);
    CHECK_FALSE(result.is_error);
    CHECK(result.content_text.substr(0, 1000) == std::string(1000, 'x'));
    CHECK(result.content_text.find("the first 1000 of 10000 bytes") != std::string::npos);

    // The footer names a project-relative path, and that file holds the FULL
    // text -- exactly what `read` would page into.
    const auto start = result.content_text.find(".minimal/web-fetch/");
    REQUIRE(start != std::string::npos);
    const auto end = result.content_text.find(" --", start);
    REQUIRE(end != std::string::npos);
    std::ifstream spill(root / result.content_text.substr(start, end - start));
    const std::string spilled((std::istreambuf_iterator<char>(spill)),
                              std::istreambuf_iterator<char>());
    CHECK(spilled == body);

    std::filesystem::remove_all(root);
}

TEST_CASE("web_fetch: truncation backs up to a UTF-8 boundary, never splits a codepoint",
          "[tools_web][fetch]") {
    // Regression: a raw byte cut could land inside a multi-byte UTF-8
    // sequence, emitting an invalid trailing byte into the model's context.
    const std::string euro = "\xE2\x82\xAC";  // one 3-byte codepoint
    const std::string body = std::string(9, 'x') + euro + std::string(100, 'y');
    LoopbackServer server({HttpResponseOf(200, "OK", "text/plain", body)});
    WebSettings settings = LocalSettings();
    settings.fetch_default_max_bytes = 10;  // cuts right after the euro's first byte
    auto tool = MakeWebFetchTool(settings, net::HttpClient{});

    const auto root = std::filesystem::temp_directory_path() / "minimal-webfetch-utf8-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    auto ctx = Context();
    ctx.project_root = root;

    const auto result = tool->execute({{"url", server.url()}}, ctx);
    CHECK_FALSE(result.is_error);
    // The whole partial codepoint is dropped, not a truncated prefix of it.
    CHECK(result.content_text.substr(0, 9) == std::string(9, 'x'));
    CHECK(result.content_text.find('\xE2') == std::string::npos);

    std::filesystem::remove_all(root);
}

TEST_CASE("web_fetch: a response under the cap is returned whole, with no spill",
          "[tools_web][fetch]") {
    LoopbackServer server({HttpResponseOf(200, "OK", "text/plain", "hello world")});
    WebSettings settings = LocalSettings();
    settings.fetch_default_max_bytes = 100000;
    auto tool = MakeWebFetchTool(settings, net::HttpClient{});

    const auto root = std::filesystem::temp_directory_path() / "minimal-webfetch-nospill-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    auto ctx = Context();
    ctx.project_root = root;

    const auto result = tool->execute({{"url", server.url()}}, ctx);
    CHECK_FALSE(result.is_error);
    CHECK(result.content_text == "hello world");
    CHECK_FALSE(std::filesystem::exists(root / ".minimal"));

    std::filesystem::remove_all(root);
}

TEST_CASE("web_fetch: max_bytes is honoured, clamped, and type-checked",
          "[tools_web][fetch]") {
    const std::string body(10000, 'y');
    LoopbackServer server({HttpResponseOf(200, "OK", "text/plain", body)});

    const auto root = std::filesystem::temp_directory_path() / "minimal-webfetch-clamp-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    auto ctx = Context();
    ctx.project_root = root;

    WebSettings settings = LocalSettings();
    settings.fetch_default_max_bytes = 400;
    settings.fetch_max_bytes = 500;
    auto tool = MakeWebFetchTool(settings, net::HttpClient{});

    SECTION("a smaller value wins over the default") {
        const auto result = tool->execute({{"url", server.url()}, {"max_bytes", 200}}, ctx);
        CHECK_FALSE(result.is_error);
        CHECK(result.content_text.substr(0, 200) == std::string(200, 'y'));
        CHECK(result.content_text.find("the first 200 of 10000 bytes") != std::string::npos);
    }
    SECTION("zero means the ceiling, not everything") {
        const auto result = tool->execute({{"url", server.url()}, {"max_bytes", 0}}, ctx);
        CHECK_FALSE(result.is_error);
        CHECK(result.content_text.find("the first 500 of 10000 bytes") != std::string::npos);
    }
    SECTION("a value past the ceiling is clamped, not honoured") {
        const auto result = tool->execute({{"url", server.url()}, {"max_bytes", 99999}}, ctx);
        CHECK_FALSE(result.is_error);
        CHECK(result.content_text.find("the first 500 of 10000 bytes") != std::string::npos);
    }
    SECTION("a non-integer is a named error") {
        const auto result = tool->execute({{"url", server.url()}, {"max_bytes", "lots"}}, ctx);
        CHECK(result.is_error);
        CHECK(result.content_text.find("'max_bytes' must be an integer") != std::string::npos);
    }

    std::filesystem::remove_all(root);
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

TEST_CASE("web_search: a row with a wrong-typed field degrades, not fails the whole search",
         "[tools_web][search]") {
    // Regression: result.value("snippet", "") throws nlohmann type_error.302
    // when the field is PRESENT but the wrong type (null instead of a
    // string), unlike the existing non-object-row guard -- one malformed
    // row used to take every good result down with it.
    LoopbackServer server({HttpResponseOf(
        200, "OK", "application/json",
        R"({"results":[
              {"title":"Good","url":"https://example.com/good","snippet":"fine"},
              {"title":"Bad","url":"https://example.com/bad","snippet":null},
              {"title":123,"url":"https://example.com/badtitle","snippet":"x"}
            ]})")});

    WebSettings settings;
    settings.search_api_url = server.url();
    auto tool = MakeWebSearchTool(settings, net::HttpClient{});
    auto ctx = Context();
    const auto result = tool->execute({{"query", "test"}}, ctx);
    CHECK_FALSE(result.is_error);
    CHECK(result.content_text.find("Good") != std::string::npos);
    CHECK(result.content_text.find("example.com/bad") != std::string::npos);
    CHECK(result.content_text.find("example.com/badtitle") != std::string::npos);
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
    CHECK(settings.fetch_default_max_bytes == 262144);
    CHECK(settings.fetch_max_bytes == 2097152);
    CHECK(settings.search_api_url.empty());
}

TEST_CASE("ParseWebSettings: a fetch.max_bytes below the default is raised, not honoured",
          "[tools_web][settings]") {
    const nlohmann::json config = {{"fetch", {{"default_max_bytes", 1024}, {"max_bytes", 512}}}};
    const auto settings = ParseWebSettings(config);
    CHECK(settings.fetch_default_max_bytes == 1024);
    CHECK(settings.fetch_max_bytes == 1024);
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

TEST_CASE("ParseWebSettings: a fractional or absurdly large value falls back, never wraps",
          "[tools_web][settings]") {
    // Regression: is_number() accepted a float (1500.7, silently truncated
    // by get<T>()) and an unclamped get<T>() narrowing let a huge value
    // (4294967297) wrap to 1 when cast to `int` -- both instead of falling
    // back to the struct default.
    const auto defaults = ParseWebSettings(nlohmann::json::object());
    const auto fractional =
        ParseWebSettings(nlohmann::json{{"fetch", {{"timeout_ms", 1500.7}}}});
    CHECK(fractional.fetch_timeout_ms == defaults.fetch_timeout_ms);
    const auto huge =
        ParseWebSettings(nlohmann::json{{"fetch", {{"max_redirects", 4294967297LL}}}});
    CHECK(huge.fetch_max_redirects > 0);  // never the wrap-to-1 the old code produced
    CHECK(huge.fetch_max_redirects == std::numeric_limits<int>::max());  // clamped, not wrapped
}
