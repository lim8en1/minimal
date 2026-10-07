#include "minimal/plugins/config_check.hpp"

#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <sstream>

#include "minimal/agent/session_support_service.hpp"
#include "minimal/common/http_status.hpp"
#include "minimal/common/policy.hpp"
#include "minimal/config/secrets.hpp"
#include "minimal/net/http_client.hpp"
#include "minimal/plugins/service_registry.hpp"

namespace minimal::plugins {
namespace {

// "host:port" of a URL, for messages: what the user would recognise.
std::string HostOf(const std::string& url) {
    const auto scheme = url.find("://");
    const std::size_t start = scheme == std::string::npos ? 0 : scheme + 3;
    const auto end = url.find('/', start);
    return url.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

std::string StatusProblem(int status) {
    const std::string code = "HTTP " + std::to_string(status);
    if (status == 401 || status == 403) return code + ": the key was refused";   // NOLINT(readability-magic-numbers): HTTP status codes
    if (status == 404) return code + ": nothing at this URL -- a typo in the path?";   // NOLINT(readability-magic-numbers)
    if (status == 405) return code + ": this URL takes another method -- the wrong endpoint?";   // NOLINT(readability-magic-numbers)
    if (status >= 500) return code + ": the server failed";   // NOLINT(readability-magic-numbers)
    return code;
}

}  // namespace

ProbeResult ProbeHttp(const std::string& url, const std::string& bearer,
                      std::chrono::milliseconds timeout, const std::function<bool()>& cancelled,
                      bool expect_json, const std::optional<std::string>& post_body) {
    ProbeResult result;
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
        result.problem = "not an http(s) URL";
        return result;
    }
    if (cancelled && cancelled()) {
        result.problem = "cancelled";
        return result;
    }
    net::HttpRequest request;
    request.url = url;
    request.request_timeout = timeout;
    request.connect_timeout = std::min(timeout, common::kDefaultConnectTimeout);
    request.should_abort = cancelled;
    if (!bearer.empty()) request.headers.push_back({"Authorization", "Bearer " + bearer});
    if (post_body) {
        request.headers.push_back({"Content-Type", "application/json"});
        request.body = *post_body;
    }
    net::HttpResponse response;
    try {
        const net::HttpClient client;
        response = post_body ? client.Post(request) : client.Get(request);
    } catch (const net::HttpError& error) {
        switch (error.kind()) {
            case net::HttpErrorKind::ConnectFailed:
                result.problem = "cannot connect to " + HostOf(url) + " (" + error.what() + ")";
                break;
            case net::HttpErrorKind::TlsError:
                result.problem = "TLS failed with " + HostOf(url) + " (" + error.what() + ")";
                break;
            case net::HttpErrorKind::Timeout:
                result.problem = HostOf(url) + " did not answer in time";
                break;
            case net::HttpErrorKind::Cancelled:
                result.problem = "cancelled";
                break;
            case net::HttpErrorKind::Other:
                result.problem = error.what();
                break;
        }
        return result;
    } catch (const std::exception& error) {
        result.problem = error.what();
        return result;
    }
    result.status = response.status;
    result.body = nlohmann::json::parse(response.body, nullptr, /*allow_exceptions=*/false);
    if (result.body.is_discarded()) result.body = nullptr;
    if (!common::IsSuccessStatus(response.status)) {
        result.problem = StatusProblem(response.status);
        if (result.body.is_object()) {
            // An API's own words, when it gave some ({"error": {"message"}}).
            const auto& error = result.body.contains("error") ? result.body.at("error") : result.body;
            if (error.is_object() && error.contains("message") && error.at("message").is_string()) {
                result.problem += " (" + error.at("message").get<std::string>() + ")";
            } else if (error.is_string()) {
                result.problem += " (" + error.get<std::string>() + ")";
            }
        }
        return result;
    }
    if (expect_json && result.body.is_null()) {
        result.problem = "answered, but not as an API (not JSON) -- the wrong URL?";
        return result;
    }
    result.ok = true;
    return result;
}

std::optional<std::vector<std::string>> ListOpenAiModels(const std::string& base_url,
                                                         const std::string& bearer,
                                                         std::chrono::milliseconds timeout,
                                                         const std::function<bool()>& cancelled,
                                                         std::string& problem) {
    std::string base = base_url;
    while (!base.empty() && base.back() == '/') base.pop_back();
    const ProbeResult probe = ProbeHttp(base + "/models", bearer, timeout, cancelled);
    if (!probe.ok) {
        problem = probe.problem;
        return std::nullopt;
    }
    const auto data = probe.body.is_object() ? probe.body.find("data") : probe.body.end();
    if (!probe.body.is_object() || data == probe.body.end() || !data->is_array()) {
        problem = "answered, but not with a model list ({\"data\": [...]}) -- not an OpenAI-shaped API?";
        return std::nullopt;
    }
    std::vector<std::string> ids;
    for (const auto& model : *data) {
        if (model.is_object() && model.contains("id") && model.at("id").is_string()) {
            ids.push_back(model.at("id").get<std::string>());
        }
    }
    return ids;
}

std::optional<std::string> ModelNotServed(const std::string& model,
                                          const std::vector<std::string>& served) {
    if (std::find(served.begin(), served.end(), model) != served.end()) return std::nullopt;
    if (served.empty()) return "not served here (it lists no models)";
    std::string list;
    constexpr std::size_t kShown = 6;  // NOLINT(readability-magic-numbers): enough to spot a near-miss, short enough to read
    for (std::size_t i = 0; i < served.size() && i < kShown; ++i) {
        list += (list.empty() ? "" : ", ") + served[i];
    }
    if (served.size() > kShown) list += ", … (" + std::to_string(served.size()) + " in all)";
    return "not served here (it serves: " + list + ")";
}

std::optional<std::string> ModelRefProblem(const ServiceRegistry* services, const std::string& ref) {
    if (services == nullptr) return "not checked: the plugin is not running";
    auto* support = static_cast<agent::SessionSupportService*>(
        services->Find(agent::kSessionSupportServiceName));
    if (support == nullptr) return "not checked: the server offers no model resolution";
    const std::string problem = support->ModelRefProblem(ref);
    return problem.empty() ? std::nullopt : std::optional<std::string>(problem);
}

std::optional<std::string> ResolveKey(const ServiceRegistry* services, const std::string& ref,
                                      const std::string& url, std::string& value) {
    value.clear();
    if (ref.empty()) return std::nullopt;
    try {
        const config::SecretResolver* resolver =
            services != nullptr ? static_cast<const config::SecretResolver*>(
                                      services->Find(config::kSecretsServiceName))
                                : nullptr;
        const config::SecretResolver fallback;
        const auto handle = (resolver != nullptr ? *resolver : fallback).Bind(ref, url);
        if (handle.configured()) value = handle.Get().value;
        return std::nullopt;
    } catch (const std::exception& error) {
        return std::string(error.what());
    }
}

std::optional<std::string> ExecutableProblem(const std::string& program) {
    if (program.empty()) return "no program named";
    const auto runnable = [](const std::filesystem::path& path) {
        std::error_code ec;
        return std::filesystem::is_regular_file(path, ec) && ::access(path.c_str(), X_OK) == 0;
    };
    if (program.find('/') != std::string::npos) {
        std::error_code ec;
        if (!std::filesystem::exists(program, ec)) return "no such file: " + program;
        if (!runnable(program)) return program + " is not executable";
        return std::nullopt;
    }
    const char* path = std::getenv("PATH");
    std::stringstream dirs(path != nullptr ? path : "");
    for (std::string dir; std::getline(dirs, dir, ':');) {
        if (!dir.empty() && runnable(std::filesystem::path(dir) / program)) return std::nullopt;
    }
    return program + " is not on PATH";
}

}  // namespace minimal::plugins
