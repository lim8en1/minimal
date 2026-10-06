// tools-web: URL policy (SSRF guard), HTML->text extraction, and the two
// tools (web_fetch, web_search). The tool tests run against a real loopback
// HTTP server (support/loopback_http_server.hpp) -- exactly the layer a fake
// would paper over.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <limits>
#include <string>
#include <thread>

#include "fetch_tool.hpp"
#include "html_text.hpp"
#include "loopback_http_server.hpp"
#include "search_tool.hpp"
#include "url_policy.hpp"
#include "web_settings.hpp"

using namespace minimal;
using namespace minimal::tools_web;

namespace {

using LoopbackServer = minimal::test::LoopbackHttpServer;
using minimal::test::HttpResponseOf;

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

TEST_CASE("HtmlToText: a body of bare '&' with no ';' anywhere does not scan the whole "
         "document per '&'",
         "[tools_web][html_text][invariant]") {
    // DecodeEntity's ';' search used to be unbounded (`html.find(';', pos +
    // 1)` over the FULL string), with only the RESULT compared against the
    // 13-byte entity window afterward -- so a body of mostly '&' with no ';'
    // anywhere scanned from each '&' to end-of-document, O(N^2). Measured on
    // the unfixed code: ~0.1s for 100k '&', ~1.1s for 400k, extrapolating to
    // minutes at real page sizes. Bounded to a comfortably-generous ceiling
    // that the fixed O(N) behavior clears trivially but the old O(N^2)
    // behavior does not (800k chars extrapolates to several seconds under
    // the bug).
    const std::string body(800000, '&');
    const auto start = std::chrono::steady_clock::now();
    const auto text = HtmlToText(body);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(text.size() == body.size());  // every '&' survives as a literal
    CHECK(elapsed < std::chrono::seconds(2));
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

TEST_CASE("web_fetch: a redirect to another host is handed back, not followed",
          "[tools_web][fetch]") {
    // Permission was decided for this host; the other host's fetch must be
    // its own gated call. (127.0.0.1 -> localhost: same machine, other host.)
    LoopbackServer redirector({HttpResponseOf(302, "Found", "text/plain", "",
                                              {"Location: http://localhost:9/elsewhere"})});
    auto tool = MakeWebFetchTool(LocalSettings(), net::HttpClient{});
    auto ctx = Context();
    const auto result = tool->execute({{"url", redirector.url()}}, ctx);
    CHECK(result.is_error);
    CHECK(result.content_text.find("to another host: http://localhost:9/elsewhere") !=
          std::string::npos);
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

TEST_CASE("web_search: SearXNG's 'content' is the snippet; 'snippet' wins when both are present",
         "[tools_web][search]") {
    LoopbackServer server({HttpResponseOf(
        200, "OK", "application/json",
        R"({"query":"test","number_of_results":2,"results":[
              {"title":"Searx","url":"https://example.com/a","content":"from content","engine":"ddg"},
              {"title":"Both","url":"https://example.com/b","content":"loser","snippet":"winner"}
            ]})")});

    WebSettings settings;
    settings.search_api_url = server.url() + "?format=json";
    auto tool = MakeWebSearchTool(settings, net::HttpClient{});
    auto ctx = Context();
    const auto result = tool->execute({{"query", "test"}}, ctx);
    CHECK_FALSE(result.is_error);
    CHECK(result.content_text.find("from content") != std::string::npos);
    CHECK(result.content_text.find("winner") != std::string::npos);
    CHECK(result.content_text.find("loser") == std::string::npos);
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

TEST_CASE("ParseWebSettings: an unknown field is a named error at either level",
          "[tools_web][settings]") {
    CHECK_THROWS_WITH(ParseWebSettings({{"serch", nlohmann::json::object()}}),
                      Catch::Matchers::ContainsSubstring("tools-web.config.serch: unknown field"));
    CHECK_THROWS_WITH(ParseWebSettings({{"search", {{"api_key_env", "K"}}}}),
                      Catch::Matchers::ContainsSubstring(
                          "tools-web.config.search.api_key_env: unknown field"));
}

TEST_CASE("ParseWebSettings: a fetch.max_bytes below the default is refused",
          "[tools_web][settings]") {
    const nlohmann::json config = {{"fetch", {{"default_max_bytes", 1024}, {"max_bytes", 512}}}};
    CHECK_THROWS_WITH(ParseWebSettings(config),
                      Catch::Matchers::ContainsSubstring(
                          "fetch.max_bytes: must not be below fetch.default_max_bytes"));
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

TEST_CASE("ParseWebSettings: an unset env: search.api_key binds and fails per use",
          "[tools_web][settings]") {
    const nlohmann::json config = {{"search", {{"api_key", "env:MINIMAL_TEST_UNSET_VAR_XYZ"}}}};
    const auto settings = ParseWebSettings(config);
    CHECK_THROWS_WITH(settings.search_api_key.Get(),
                      Catch::Matchers::ContainsSubstring("MINIMAL_TEST_UNSET_VAR_XYZ"));
    CHECK_THROWS_AS(ParseWebSettings({{"search", {{"api_key", "secrets:typo"}}}}),
                    std::runtime_error);
}

TEST_CASE("ParseWebSettings: a fractional or absurdly large value is refused, never wrapped",
          "[tools_web][settings]") {
    using Catch::Matchers::ContainsSubstring;
    CHECK_THROWS_WITH(ParseWebSettings(nlohmann::json{{"fetch", {{"timeout_ms", 1500.7}}}}),
                      ContainsSubstring("fetch.timeout_ms: must be an integer"));
    CHECK_THROWS_WITH(
        ParseWebSettings(nlohmann::json{{"fetch", {{"max_redirects", 4294967297LL}}}}),
        ContainsSubstring("fetch.max_redirects: must be at most"));
}
