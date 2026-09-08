#include "dbgd_client.hpp"

#include <utility>

#include "minimal/net/http_client.hpp"

namespace minimal::tools_windbg {

DbgdError DbgdClient::MapProtocol(const nlohmann::json& payload) {
    if (!payload.is_object()) {
        return DbgdError{"protocol", "response is not a JSON object", "", 0};
    }
    const auto version = payload.find("protocol_version");
    if (version == payload.end() || !version->is_number_integer()) {
        return DbgdError{"protocol", "response carries no protocol_version", "", 0};
    }
    if (version->get<int>() != settings_.protocol_version) {
        // The two-sided stale-plugin guard: refuse before interpreting any
        // payload the other side may have shaped differently.
        return DbgdError{"version_mismatch",
                         "dbgd speaks protocol " + std::to_string(version->get<int>()) +
                             ", this tool expects " + std::to_string(settings_.protocol_version),
                         "upgrade dbgd (dbg-cli) or adjust the plugin's protocol_version "
                         "config to match", 0};
    }
    return DbgdError{};  // code empty = clean
}

std::string DbgdClient::Query(const std::string& path,
                              std::vector<std::pair<std::string, std::string>> params) const {
    std::string out = path;
    bool first = true;
    for (const auto& [key, value] : params) {
        out += first ? '?' : '&';
        first = false;
        out += key + '=' + value;
    }
    return out;
}

nlohmann::json DbgdClient::Request(const std::string& method, const std::string& path,
                                   const nlohmann::json* body, DbgdError* error) {
    net::HttpRequest request;
    request.url = settings_.base_url + path;
    request.headers.push_back({"Content-Type", "application/json"});
    request.headers.push_back({"User-Agent", "minimal-windbg/0.1"});
    request.headers.push_back({"X-Dbg-Protocol", std::to_string(settings_.protocol_version)});
    if (!settings_.bearer_token.empty()) {
        request.headers.push_back({"Authorization", "Bearer " + settings_.bearer_token});
    }
    if (body != nullptr) request.body = body->dump();
    request.connect_timeout = std::chrono::milliseconds(5000);
    request.request_timeout = std::chrono::milliseconds(settings_.request_timeout_ms);

    net::HttpResponse response;
    try {
        if (method == "POST") {
            response = http_.Post(request);
        } else if (method == "GET") {
            response = http_.Get(request);
        } else if (method == "DELETE") {
            response = http_.Delete(request);
        } else {
            if (error != nullptr) {
                *error = DbgdError{"protocol", "unsupported method " + method, "", 0};
            }
            return {};
        }
    } catch (const net::HttpError& e) {
        if (error != nullptr) {
            *error = DbgdError{"transport", e.what(),
                               "is dbgd running at " + settings_.base_url +
                                   "? start it on the debugger host (dbg-cli: `dbgd`)",
                               0};
        }
        return {};
    }

    auto payload = nlohmann::json::parse(response.body, nullptr, false);
    if (payload.is_discarded()) {
        if (error != nullptr) {
            *error = DbgdError{"protocol",
                               "response is not valid JSON (status " +
                                   std::to_string(response.status) + ")",
                               "", response.status};
        }
        return {};
    }
    if (response.status < 200 || response.status >= 300) {
        // The error body: {"error": {code, message, hint}}.
        if (error != nullptr) {
            const auto err = payload.find("error");
            if (err != payload.end() && err->is_object()) {
                error->code = err->value("code", std::string("internal"));
                error->message = err->value("message", std::string());
                error->hint = err->value("hint", std::string());
            } else {
                error->code = "internal";
                error->message = "HTTP " + std::to_string(response.status);
            }
            error->status = response.status;
        }
        return {};
    }
    const auto protocol_error = MapProtocol(payload);
    if (!protocol_error.code.empty()) {
        if (error != nullptr) *error = protocol_error;
        return {};
    }
    return payload;
}

std::string DbgdClient::Connect(const nlohmann::json& target, DbgdError* error) {
    nlohmann::json body{{"target", target}, {"backend", settings_.backend}};
    const auto payload = Request("POST", "/v1/sessions", &body, error);
    if (payload.is_null()) return {};
    return payload.value("handle", std::string());
}

ExecResult DbgdClient::Exec(const std::string& handle, const std::string& command,
                            bool allow_running, DbgdError* error) {
    nlohmann::json body{{"command", command},
                        {"allow_running", allow_running},
                        {"grace", static_cast<double>(settings_.exec_grace_s)},
                        {"max_bytes", settings_.max_output_bytes}};
    const auto payload =
        Request("POST", "/v1/sessions/" + handle + "/exec", &body, error);
    if (payload.is_null()) return {};

    ExecResult result;
    result.ok = payload.value("ok", false);
    result.output = payload.value("output", std::string());
    result.error_text = payload.value("error_text", std::string());
    if (payload.contains("state")) result.state = payload.at("state");
    result.job_id = payload.value("job_id", std::string());
    return result;
}

JobChunk DbgdClient::ReadJob(const std::string& handle, const std::string& job_id,
                            std::int64_t since, DbgdError* error) {
    (void)handle;  // the job route keys on the job id; the handle rides in the response
    const auto path = Query(
        "/v1/jobs/" + job_id,
        {{"since", std::to_string(since)},
         {"max_bytes", std::to_string(settings_.max_output_bytes)},
         {"wait", std::to_string(settings_.job_wait_s)}});
    const auto payload = Request("GET", path, nullptr, error);
    if (payload.is_null()) return {};

    JobChunk out;
    out.done = payload.value("done", false);
    out.output = payload.value("output", std::string());
    out.next_offset = payload.value("next_offset", static_cast<std::int64_t>(0));
    out.truncated = payload.value("truncated", false);
    if (payload.contains("state")) out.state = payload.at("state");
    return out;
}

nlohmann::json DbgdClient::Interrupt(const std::string& handle, DbgdError* error) {
    return Request("POST", "/v1/sessions/" + handle + "/interrupt", nullptr, error);
}

nlohmann::json DbgdClient::State(const std::string& handle, DbgdError* error) {
    return Request("GET", "/v1/sessions/" + handle + "/state", nullptr, error);
}

void DbgdClient::Close(const std::string& handle, const std::string& on_exit) {
    // DELETE /v1/sessions/{handle}, with the on_exit disposition in the body
    // (dbgd reads it there). Idempotent by contract: a second close comes
    // back as session_gone, which callers treat as success. Best-effort:
    // failures are returned to whoever cares -- the lifetime watcher and the
    // session-end observer both log, neither can do anything about it.
    nlohmann::json body{{"on_exit", on_exit}};
    DbgdError error;
    const auto payload =
        Request("DELETE", "/v1/sessions/" + handle, &body, &error);
    (void)payload;
    (void)error;
}

}  // namespace minimal::tools_windbg
