#include "search_tool.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <limits>
#include <sstream>

#include <nlohmann/json.hpp>

#include "url_policy.hpp"

namespace minimal::tools_web {
namespace {

using tools::ToolCategory;
using tools::ToolContext;
using tools::ToolResult;
using tools::ToolSpec;

ToolResult Error(std::string message) { return ToolResult{true, std::move(message)}; }

// Like nlohmann's own `j.value(key, fallback)`, but never throws:
// `.value()` raises type_error.302 when the field is present with the WRONG
// type (a null "snippet" instead of a string), which -- since these fields
// come from a whole ARRAY of rows processed in one loop -- used to fail the
// entire search over one malformed row, taking every good result down with
// it, exactly the failure mode the surrounding comment already guards
// against for a non-object row.
std::string SafeString(const nlohmann::json& j, const char* key, std::string fallback) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return fallback;
    return it->get<std::string>();
}

std::string PercentEncode(std::string_view text) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size());
    for (const unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 0x0F];
        }
    }
    return out;
}

class WebSearchTool : public tools::Tool {
public:
    WebSearchTool(WebSettings settings, net::HttpClient http)
        : settings_(std::move(settings)), http_(std::move(http)) {
        spec_.name = "web_search";
        // ReadOnly: it runs nothing locally. The network dimension
        // (network_access_required) is its gate -- as Execute it also
        // needed `execute: allow`, which allows bash, to run unprompted.
        spec_.category = ToolCategory::ReadOnly;
        spec_.definition.name = spec_.name;
        spec_.definition.description =
            "Search the web and return a list of results (title, URL, snippet). Requires a "
            "search backend to be configured (tools-web plugin config: search.api_url); "
            "reports plainly when it is not.";
        spec_.definition.input_schema = {
            {"type", "object"},
            {"properties",
              {{"query", {{"type", "string"}, {"description", "The search query"}}},
               {"max_results",
                {{"type", "integer"},
                 {"description", "How many results to return (default " +
                                     std::to_string(settings_.search_default_max_results) +
                                     ", capped at " + std::to_string(settings_.search_max_results) +
                                     ")"}}}}},
            {"required", nlohmann::json::array({"query"})}};
    }

    const ToolSpec& spec() const override { return spec_; }
    bool network_access_required() const override { return true; }
    std::vector<std::string> config_aliases() const override { return {"websearch"}; }
    bool accepts_pattern_rules() const override { return false; }

    ToolResult execute(const nlohmann::json& input, ToolContext& ctx) override {
        if (settings_.search_api_url.empty()) {
            return Error(
                "web_search: not configured -- set tools-web's plugin config "
                "search.api_url (see bundled/providers.example.json-style docs)");
        }
        if (!input.contains("query") || !input.at("query").is_string()) {
            return Error("web_search: 'query' is required");
        }
        const std::string query = input.at("query").get<std::string>();

        int max_results = settings_.search_default_max_results;
        if (input.contains("max_results") && !input.at("max_results").is_number_integer()) {
            // Rejected, not silently defaulted: a model that passed a string
            // ("5") would otherwise get the default count believing its
            // request had taken effect. Same posture as web_fetch's
            // max_bytes type check.
            return Error("web_search: 'max_results' must be an integer");
        }
        if (input.contains("max_results")) {
            // Same narrowing discipline as web_fetch's max_bytes: get<int>()
            // after only an is_number_integer check silently truncated (or
            // wrapped) a value outside int range before the clamp could run.
            const auto raw = input.at("max_results").get<std::int64_t>();
            if (raw < 1 || raw > std::numeric_limits<int>::max()) {
                return Error("web_search: 'max_results' is out of range");
            }
            max_results = static_cast<int>(raw);
        }
        max_results = std::clamp(max_results, 1, settings_.search_max_results);

        if (ctx.is_cancelled && ctx.is_cancelled()) return Error("web_search: cancelled");

        const UrlCheck check = CheckUrl(settings_.search_api_url, /*allow_private_hosts=*/true);
        // allow_private_hosts=true here: the search BACKEND is an operator's
        // own configured endpoint (often a local SearXNG instance), not a
        // URL the model chose -- unlike web_fetch, there is nothing to guard
        // the operator's own choice against. The scheme check still applies.
        if (!check.ok) return Error("web_search: refused (" + check.reason + ")");

        std::string url = settings_.search_api_url;
        url += (url.find('?') == std::string::npos) ? '?' : '&';
        url += "q=" + PercentEncode(query) + "&count=" + std::to_string(max_results);

        net::HttpRequest request;
        request.url = url;
        request.request_timeout = std::chrono::milliseconds(settings_.search_timeout_ms);
        if (settings_.search_api_key.configured()) {
            try {
                request.headers.push_back(
                    {"Authorization", "Bearer " + settings_.search_api_key.Get().value});
            } catch (const std::exception& error) {
                return Error(std::string("web_search: api key: ") + error.what());
            }
        }

        net::HttpResponse response;
        try {
            response = http_.Get(request);
        } catch (const net::HttpError& error) {
            return Error(std::string("web_search: ") + error.what());
        }
        if (response.status < 200 || response.status >= 300) {
            return Error("web_search: backend returned HTTP " + std::to_string(response.status));
        }

        nlohmann::json body;
        try {
            body = nlohmann::json::parse(response.body);
        } catch (const nlohmann::json::exception& error) {
            return Error(std::string("web_search: backend response was not JSON: ") + error.what());
        }
        if (!body.contains("results") || !body.at("results").is_array()) {
            return Error(
                "web_search: backend response has no \"results\" array -- expected "
                "{\"results\": [{\"title\",\"url\",\"snippet\"}, ...]}");
        }

        std::ostringstream out;
        int count = 0;
        for (const auto& result : body.at("results")) {
            if (count >= max_results) break;
            // Non-object elements are skipped, not fatal: the nlohmann
            // value() calls below would throw out of the tool and fail the
            // whole search over one malformed row, taking every good result
            // down with it.
            if (!result.is_object()) continue;
            const std::string title = SafeString(result, "title", "");
            const std::string result_url = SafeString(result, "url", "");
            // SearXNG names the snippet "content"; "snippet" wins when both are present.
            const std::string snippet = SafeString(result, "snippet", SafeString(result, "content", ""));
            if (result_url.empty()) continue;   // a result naming no URL is useless to the model
            out << (count + 1) << ". " << (title.empty() ? result_url : title) << "\n" << result_url;
            if (!snippet.empty()) out << "\n" << snippet;
            out << "\n\n";
            ++count;
        }
        if (count == 0) return ToolResult{false, "no results"};
        return ToolResult{false, out.str()};
    }

private:
    ToolSpec spec_;
    WebSettings settings_;
    net::HttpClient http_;  // stateless -- see HttpClient's own contract
};

}  // namespace

std::unique_ptr<tools::Tool> MakeWebSearchTool(WebSettings settings, net::HttpClient http) {
    return std::make_unique<WebSearchTool>(std::move(settings), std::move(http));
}

}  // namespace minimal::tools_web
