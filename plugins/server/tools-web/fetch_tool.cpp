#include "fetch_tool.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <limits>

#include "html_text.hpp"
#include "minimal/tools/atomic_write.hpp"
#include "url_policy.hpp"

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
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char c : url) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    char buffer[17];
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
    WebFetchTool(WebSettings settings, net::HttpClient http)
        : settings_(std::move(settings)), http_(std::move(http)) {
        spec_.name = "web_fetch";
        spec_.category = ToolCategory::Execute;
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
                {"description",
                 "Maximum bytes of text to return; default " +
                     std::to_string(settings_.fetch_default_max_bytes) + ", capped at " +
                     std::to_string(settings_.fetch_max_bytes) + ". Anything over the limit is "
                     "saved to a file you can read with the read tool -- the result footer "
                     "names it."}}}}},
            {"required", nlohmann::json::array({"url"})}};
    }

    const ToolSpec& spec() const override { return spec_; }
    bool network_access_required() const override { return true; }

    ToolResult execute(const nlohmann::json& input, ToolContext& ctx) override {
        if (!input.contains("url") || !input.at("url").is_string()) {
            return Error("web_fetch: 'url' is required");
        }
        std::string url = input.at("url").get<std::string>();

        // Clamped like read's window: 0 and over-the-ceiling both mean the
        // ceiling (disclosed in the footer), an out-of-range value is the
        // model's mistake to see, not a silent wrap.
        int max_bytes = settings_.fetch_default_max_bytes;
        if (input.contains("max_bytes")) {
            if (!input.at("max_bytes").is_number_integer()) {
                return Error("web_fetch: 'max_bytes' must be an integer");
            }
            const auto raw = input.at("max_bytes").get<std::int64_t>();
            if (raw < 0 || raw > std::numeric_limits<int>::max()) {
                return Error("web_fetch: 'max_bytes' is out of range");
            }
            max_bytes = raw == 0 ? settings_.fetch_max_bytes
                                 : static_cast<int>(std::min<std::int64_t>(
                                       raw, settings_.fetch_max_bytes));
        }

        for (int hop = 0; hop <= settings_.fetch_max_redirects; ++hop) {
            if (ctx.is_cancelled && ctx.is_cancelled()) return Error("web_fetch: cancelled");

            const UrlCheck check = CheckUrl(url, settings_.fetch_allow_private_hosts);
            if (!check.ok) return Error("web_fetch: refused (" + check.reason + ")");

            net::HttpRequest request;
            request.url = url;
            request.request_timeout = std::chrono::milliseconds(settings_.fetch_timeout_ms);
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

            if (response.status >= 300 && response.status < 400) {
                const auto next = AbsoluteRedirectTarget(response.headers);
                if (!next) {
                    return Error("web_fetch: redirect (HTTP " + std::to_string(response.status) +
                                ") with no absolute Location header to follow");
                }
                url = *next;
                continue;
            }

            if (response.status < 200 || response.status >= 300) {
                return Error("web_fetch: HTTP " + std::to_string(response.status));
            }

            return Finish(ctx, url, ContentTypeIsHtml(response.headers)
                                        ? HtmlToText(response.body)
                                        : response.body,
                          max_bytes);
        }
        return Error("web_fetch: too many redirects (" +
                     std::to_string(settings_.fetch_max_redirects) + ")");
    }

private:
    // Bounds the returned text. The overflow is spilled to a project-local
    // file: the model reads only text, so "there is more" has to be a PATH it
    // can hand to the read tool (which confines to the project root), not a
    // structured handle. The spill name is server-named (URL hash), never
    // model-named, and the same URL overwrites its file instead of
    // accumulating one per fetch.
    ToolResult Finish(const ToolContext& ctx, const std::string& url, std::string text,
                      int max_bytes) {
        if (text.size() <= static_cast<std::size_t>(max_bytes)) {
            return ToolResult{false, std::move(text)};
        }
        const auto total = text.size();
        std::error_code ec;
        const auto dir = ctx.project_root / ".minimal" / "web-fetch";
        std::filesystem::create_directories(dir, ec);
        const std::string name = SpillName(url);
        const auto outcome = ec ? tools::WriteOutcome{} : tools::write_file(dir / name, text);
        if (outcome.ok) {
            text.resize(static_cast<std::size_t>(max_bytes));
            return ToolResult{false,
                              std::move(text) + "\n[Showing the first " +
                                  std::to_string(max_bytes) + " of " + std::to_string(total) +
                                  " bytes. The full response is saved at .minimal/web-fetch/" +
                                  name + " -- use the read tool (offset/limit) to page through "
                                  "the rest.]"};
        }
        // The spill failed: truncate anyway, and say the pointer is missing
        // rather than returning one that leads nowhere.
        text.resize(static_cast<std::size_t>(max_bytes));
        return ToolResult{false,
                          std::move(text) + "\n[Truncated to the first " +
                              std::to_string(max_bytes) + " of " + std::to_string(total) +
                              " bytes; saving the full response failed (" + outcome.error +
                              ") -- re-fetch with a larger max_bytes if you need more.]"};
    }

    ToolSpec spec_;
    WebSettings settings_;
    net::HttpClient http_;  // stateless -- see HttpClient's own contract
};

}  // namespace

std::unique_ptr<tools::Tool> MakeWebFetchTool(WebSettings settings, net::HttpClient http) {
    return std::make_unique<WebFetchTool>(std::move(settings), std::move(http));
}

}  // namespace minimal::tools_web
