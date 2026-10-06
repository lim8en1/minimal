#include "fetch_tool.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <limits>

#include "html_text.hpp"
#include "minimal/common/utf8.hpp"
#include "minimal/common/atomic_write.hpp"
#include "minimal/common/http_status.hpp"
#include "url_policy.hpp"
#include "minimal/common/fnv.hpp"

namespace minimal::tools_web {
namespace {

using tools::ToolCategory;
using tools::ToolContext;
using tools::ToolResult;
using tools::ToolSpec;

ToolResult Error(std::string message) { return ToolResult{true, std::move(message)}; }

// FNV-1a 64, hex-encoded. The spill FILENAME is a cache key, not a security
// boundary: it only has to be deterministic so the same URL overwrites its
// file instead of accumulating one per fetch.
std::string SpillName(const std::string& url) {
    const std::uint64_t hash = common::Fnv1a64(url);
    char buffer[sizeof(hash) * 2 + 1];  // two hex digits per byte, and the NUL
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(hash));
    return std::string(buffer) + ".txt";
}

bool ContentTypeIsHtml(const std::vector<net::HttpHeader>& headers) {
    const auto content_type = net::FindHeaderCI(headers, "Content-Type");
    if (!content_type) return false;
    // "text/html", "application/xhtml+xml", with or without a charset
    // parameter -- a plain substring check is enough here, this is not a
    // MIME-type validator.
    return content_type->find("html") != std::string::npos;
}

// A conservative, substring-based deny-list: enough to catch the common
// binary formats (images, archives, PDFs) that show up as U+FFFD junk (or
// raw bytes) in the model's context if returned as "text". Not exhaustive --
// an unrecognized Content-Type still passes through, same as today, so this
// only ever narrows what gets dumped raw, never blocks a legitimate text
// response the check doesn't recognize.
bool ContentTypeLooksBinary(const std::vector<net::HttpHeader>& headers) {
    const auto content_type = net::FindHeaderCI(headers, "Content-Type");
    if (!content_type) return false;
    static const char* kBinaryMarkers[] = {
        "image/",  "audio/",     "video/",  "font/",
        "application/octet-stream", "application/pdf", "application/zip",
        "application/gzip", "application/x-",
    };
    for (const char* marker : kBinaryMarkers) {
        if (content_type->find(marker) != std::string::npos) return true;
    }
    return false;
}

// An ABSOLUTE Location header only. A relative one ("/login") would need
// full RFC 3986 resolution against the current URL to follow correctly;
// getting that wrong silently (e.g. dropping the scheme/host) is worse than
// declining to follow it, so this tool reports "cannot follow" instead of
// guessing.
std::optional<std::string> AbsoluteRedirectTarget(const std::vector<net::HttpHeader>& headers) {
    const auto location = net::FindHeaderCI(headers, "Location");
    if (!location) return std::nullopt;
    if (location->rfind("http://", 0) == 0 || location->rfind("https://", 0) == 0) {
        return *location;
    }
    return std::nullopt;
}

class WebFetchTool : public tools::Tool {
public:
    WebFetchTool(std::shared_ptr<const common::LiveValue<WebSettings>> live, net::HttpClient http)
        : live_(std::move(live)), http_(std::move(http)) {
        spec_.name = "web_fetch";
        // ReadOnly: it runs nothing locally. The network dimension
        // (network_access_required) is its gate -- as Execute it also
        // needed `execute: allow`, which allows bash, to run unprompted.
        spec_.category = ToolCategory::ReadOnly;
        spec_.definition.name = spec_.name;
        spec_.definition.description =
            "Fetch a URL over HTTP(S) and return its content as text. HTML pages are converted "
            "to readable text; other content types are returned as-is. Loopback, private, and "
            "link-local addresses are refused by default. Oversized responses are truncated "
            "and the full text is saved to a file the read tool can page through.";
        spec_.definition.input_schema = {
            {"type", "object"},
            {"properties",
             {{"url", {{"type", "string"}, {"description", "The URL to fetch"}}},
              {"max_bytes",
               {{"type", "integer"},
                // No configured numbers: the definition must not change with
                // the config. The truncation footer states the limit applied.
                {"description",
                 "Maximum bytes of text to return; omitted, a default applies, and a larger "
                 "request is capped (0 = the cap). Anything over the limit is saved to a file "
                 "you can read with the read tool -- the result footer names it and the "
                 "limits."}}}}},
            {"required", nlohmann::json::array({"url"})}};
    }

    const ToolSpec& spec() const override { return spec_; }
    bool network_access_required() const override { return true; }
    // opencode's preset vocabulary spells this tool "webfetch"; its rules are
    // per host (the plugin's permission model), never core patterns.
    std::vector<std::string> config_aliases() const override { return {"webfetch"}; }
    bool accepts_pattern_rules() const override { return false; }

    ToolResult execute(const nlohmann::json& input, ToolContext& ctx) override {
        const auto settings = live_->Get();
        if (!input.contains("url") || !input.at("url").is_string()) {
            return Error("web_fetch: 'url' is required");
        }
        std::string url = input.at("url").get<std::string>();

        // Clamped like read's window: 0 and over-the-ceiling both mean the
        // ceiling (disclosed in the footer), an out-of-range value is the
        // model's mistake to see, not a silent wrap.
        int max_bytes = settings->fetch_default_max_bytes;
        // Which limit applied, for a truncation footer: the definition
        // carries no numbers, so this is where the model learns them.
        std::string limit_note = " (the default; pass max_bytes for more, up to " +
                                 std::to_string(settings->fetch_max_bytes) + ")";
        if (input.contains("max_bytes")) {
            if (!input.at("max_bytes").is_number_integer()) {
                return Error("web_fetch: 'max_bytes' must be an integer");
            }
            const auto raw = input.at("max_bytes").get<std::int64_t>();
            if (raw < 0 || raw > std::numeric_limits<int>::max()) {
                return Error("web_fetch: 'max_bytes' is out of range");
            }
            max_bytes = raw == 0 ? settings->fetch_max_bytes
                                 : static_cast<int>(std::min<std::int64_t>(
                                       raw, settings->fetch_max_bytes));
            limit_note = raw == 0 ? " (the ceiling)"
                         : raw > settings->fetch_max_bytes
                             ? " (asked for " + std::to_string(raw) + "; capped at the ceiling)"
                             : "";
        }

        for (int hop = 0; hop <= settings->fetch_max_redirects; ++hop) {
            if (ctx.is_cancelled && ctx.is_cancelled()) return Error("web_fetch: cancelled");

            const UrlCheck check = CheckUrl(url, settings->fetch_allow_private_hosts);
            if (!check.ok) return Error("web_fetch: refused (" + check.reason + ")");

            net::HttpRequest request;
            request.url = url;
            request.request_timeout = std::chrono::milliseconds(settings->fetch_timeout_ms);
            // The check just resolved this hop's host; pin the connection to
            // the addresses it vetted so libcurl's connect-time resolution
            // cannot pick a different one (rebinding TOCTOU). Empty when the
            // check skipped resolution (allow_private_hosts).
            request.pinned_resolves = check.resolve_entries;

            net::HttpResponse response;
            try {
                response = http_.Get(request);
            } catch (const net::HttpError& error) {
                return Error(std::string("web_fetch: ") + error.what());
            }

            if (common::IsRedirectStatus(response.status)) {
                const auto next = AbsoluteRedirectTarget(response.headers);
                if (!next) {
                    return Error("web_fetch: redirect (HTTP " + std::to_string(response.status) +
                                ") with no absolute Location header to follow");
                }
                // Permission was decided for THIS host (host rules, tools-web's
                // permission model): a hop to another host must be its own,
                // gated call. The model gets the target to fetch explicitly.
                // Same-host hops (http -> https, another port or path) follow.
                if (HostOf(*next) != HostOf(url)) {
                    return Error("web_fetch: redirected (HTTP " +
                                 std::to_string(response.status) + ") to another host: " + *next +
                                 " -- fetch that URL to follow it");
                }
                url = *next;
                continue;
            }

            if (!common::IsSuccessStatus(response.status)) {
                return Error("web_fetch: HTTP " + std::to_string(response.status));
            }

            // A binary body (image, archive, PDF, ...) returned as "text"
            // shows up as raw bytes or U+FFFD junk in the model's context;
            // refuse it up front rather than dumping it in, truncated or
            // not -- this tool has no rendering or extraction for it.
            if (!ContentTypeIsHtml(response.headers) && ContentTypeLooksBinary(response.headers)) {
                const auto content_type = net::FindHeaderCI(response.headers, "Content-Type");
                return Error("web_fetch: response is not text (Content-Type: " +
                             content_type.value_or("unknown") +
                             "); this tool returns text only");
            }

            return Finish(ctx, url, ContentTypeIsHtml(response.headers)
                                        ? HtmlToText(response.body)
                                        : response.body,
                          max_bytes, limit_note);
        }
        return Error("web_fetch: too many redirects (" +
                     std::to_string(settings->fetch_max_redirects) + ")");
    }

private:
    // Bounds the returned text. The overflow is spilled to a project-local
    // file: the model reads only text, so "there is more" has to be a PATH it
    // can hand to the read tool (which confines to the project root), not a
    // structured handle. The spill name is server-named (URL hash), never
    // model-named, and the same URL overwrites its file instead of
    // accumulating one per fetch.
    ToolResult Finish(const ToolContext& ctx, const std::string& url, std::string text,
                      int max_bytes, const std::string& limit_note) {
        if (text.size() <= static_cast<std::size_t>(max_bytes)) {
            return ToolResult{false, std::move(text)};
        }
        const auto total = text.size();
        std::error_code ec;
        const auto dir = ctx.project_root / ".minimal" / "web-fetch";
        std::filesystem::create_directories(dir, ec);
        const std::string name = SpillName(url);
        const auto outcome = ec ? common::WriteOutcome{} : common::write_file(dir / name, text);
        // Backed up to a UTF-8 boundary, not a raw byte cut: a cut landing
        // inside a multi-byte sequence emitted invalid UTF-8 into the
        // model's context.
        const auto safe_len = common::Utf8FloorToBoundary(text, static_cast<std::size_t>(max_bytes));
        if (outcome.ok) {
            text.resize(safe_len);
            return ToolResult{false,
                              std::move(text) + "\n[Showing the first " +
                                  std::to_string(max_bytes) + " of " + std::to_string(total) +
                                  " bytes" + limit_note +
                                  ". The full response is saved at .minimal/web-fetch/" +
                                  name + " -- use the read tool (offset/limit) to page through "
                                  "the rest.]"};
        }
        // The spill failed: truncate anyway, and say the pointer is missing
        // rather than returning one that leads nowhere.
        text.resize(safe_len);
        return ToolResult{false,
                          std::move(text) + "\n[Truncated to the first " +
                              std::to_string(max_bytes) + " of " + std::to_string(total) +
                              " bytes" + limit_note + "; saving the full response failed (" +
                              outcome.error +
                              ") -- re-fetch with a larger max_bytes if you need more.]"};
    }

    ToolSpec spec_;
    std::shared_ptr<const common::LiveValue<WebSettings>> live_;
    net::HttpClient http_;  // stateless -- see HttpClient's own contract
};

}  // namespace

std::unique_ptr<tools::Tool> MakeWebFetchTool(std::shared_ptr<const common::LiveValue<WebSettings>> live, net::HttpClient http) {
    return std::make_unique<WebFetchTool>(std::move(live), std::move(http));
}

std::unique_ptr<tools::Tool> MakeWebFetchTool(WebSettings settings, net::HttpClient http) {
    return MakeWebFetchTool(std::make_shared<common::LiveValue<WebSettings>>(std::move(settings)),
              std::move(http));
}

}  // namespace minimal::tools_web
