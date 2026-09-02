#include "fetch_tool.hpp"

#include <chrono>

#include "html_text.hpp"
#include "url_policy.hpp"

namespace minimal::tools_web {
namespace {

using tools::ToolCategory;
using tools::ToolContext;
using tools::ToolResult;
using tools::ToolSpec;

ToolResult Error(std::string message) { return ToolResult{true, std::move(message)}; }

bool ContentTypeIsHtml(const std::vector<llm::HttpHeader>& headers) {
    const auto content_type = llm::FindHeaderCI(headers, "Content-Type");
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
std::optional<std::string> AbsoluteRedirectTarget(const std::vector<llm::HttpHeader>& headers) {
    const auto location = llm::FindHeaderCI(headers, "Location");
    if (!location) return std::nullopt;
    if (location->rfind("http://", 0) == 0 || location->rfind("https://", 0) == 0) {
        return *location;
    }
    return std::nullopt;
}

class WebFetchTool : public tools::Tool {
public:
    WebFetchTool(WebSettings settings, llm::HttpClient http)
        : settings_(std::move(settings)), http_(std::move(http)) {
        spec_.name = "web_fetch";
        spec_.category = ToolCategory::Execute;
        spec_.definition.name = spec_.name;
        spec_.definition.description =
            "Fetch a URL over HTTP(S) and return its content as text. HTML pages are converted "
            "to readable text; other content types are returned as-is. Loopback, private, and "
            "link-local addresses are refused by default.";
        spec_.definition.input_schema = {
            {"type", "object"},
            {"properties", {{"url", {{"type", "string"}, {"description", "The URL to fetch"}}}}},
            {"required", nlohmann::json::array({"url"})}};
    }

    const ToolSpec& spec() const override { return spec_; }
    bool network_access_required() const override { return true; }

    ToolResult execute(const nlohmann::json& input, ToolContext& ctx) override {
        if (!input.contains("url") || !input.at("url").is_string()) {
            return Error("web_fetch: 'url' is required");
        }
        std::string url = input.at("url").get<std::string>();

        for (int hop = 0; hop <= settings_.fetch_max_redirects; ++hop) {
            if (ctx.is_cancelled && ctx.is_cancelled()) return Error("web_fetch: cancelled");

            const UrlCheck check = CheckUrl(url, settings_.fetch_allow_private_hosts);
            if (!check.ok) return Error("web_fetch: refused (" + check.reason + ")");

            llm::HttpRequest request;
            request.url = url;
            request.request_timeout = std::chrono::milliseconds(settings_.fetch_timeout_ms);

            llm::HttpResponse response;
            try {
                response = http_.Get(request);
            } catch (const llm::HttpError& error) {
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

            if (ContentTypeIsHtml(response.headers)) {
                return ToolResult{false, HtmlToText(response.body)};
            }
            return ToolResult{false, response.body};
        }
        return Error("web_fetch: too many redirects (" +
                     std::to_string(settings_.fetch_max_redirects) + ")");
    }

private:
    ToolSpec spec_;
    WebSettings settings_;
    llm::HttpClient http_;  // stateless -- see HttpClient's own contract
};

}  // namespace

std::unique_ptr<tools::Tool> MakeWebFetchTool(WebSettings settings, llm::HttpClient http) {
    return std::make_unique<WebFetchTool>(std::move(settings), std::move(http));
}

}  // namespace minimal::tools_web
