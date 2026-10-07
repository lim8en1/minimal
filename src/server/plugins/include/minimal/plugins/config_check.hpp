#pragma once

// Config checks: a plugin verifying its configuration against the world --
// the URL answers, the model is served, the key is accepted, the interpreter
// runs -- through minimal_plugin_check_config (plugin_abi.h). The host runs a
// check off every request path, after each save and on demand, and shows its
// findings on the fields they are about. Findings are information: a check
// never refuses a value (validation does), and it has no side effects beyond
// the probes it makes.
//
// The helpers below are the probes most checks need; each answers "fine" or
// a sentence a user can act on.

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace minimal::plugins {

class ServiceRegistry;

// What a check runs with.
struct ConfigCheckContext {
    // The host's services while the plugin is running; null when it is not
    // (disabled, or it failed to load): a check then does what needs no
    // host -- an HTTP probe, an executable -- and says what it skipped.
    const ServiceRegistry* services = nullptr;
    // True once the answer is no longer wanted (a newer save, the server
    // stopping): a long probe should give up.
    std::function<bool()> cancelled = [] { return false; };
    // The whole check's time; a single probe takes a share of it.
    std::chrono::milliseconds budget{0};
};

// A check's findings: what minimal_plugin_check_config writes to `out`.
// `path` names the field (its dotted path; an entry as "list[2].url" or
// "map.<name>"); "" is the plugin as a whole.
class CheckReport {
public:
    void Ok(const std::string& path, const std::string& message) { Add(path, "ok", message); }
    void Warn(const std::string& path, const std::string& message) { Add(path, "warning", message); }
    void Fail(const std::string& path, const std::string& message) { Add(path, "error", message); }
    const nlohmann::json& rows() const { return rows_; }

private:
    void Add(const std::string& path, const char* level, const std::string& message) {
        rows_.push_back({{"path", path}, {"level", level}, {"message", message}});
    }
    nlohmann::json rows_ = nlohmann::json::array();
};

// What one HTTP probe found: `ok` for a 2xx answer, else `problem` -- the
// failure in words ("cannot connect to host:port", "401: the key was
// refused", "404: nothing at this URL -- a typo in the path?"). `body` is
// the parsed answer when it was JSON, else null.
struct ProbeResult {
    bool ok = false;
    int status = 0;
    std::string problem;
    nlohmann::json body;
};

// GETs `url` (POSTs `post_body` as JSON when given), with `bearer` as
// Authorization when not empty. `expect_json`: a 2xx whose body is not JSON
// is a problem ("answered, but not as an API -- the wrong URL?").
ProbeResult ProbeHttp(const std::string& url, const std::string& bearer,
                      std::chrono::milliseconds timeout, const std::function<bool()>& cancelled,
                      bool expect_json = true, const std::optional<std::string>& post_body = {});

// The model ids an OpenAI-compatible API lists (GET <base>/models), or
// nullopt with `problem` set.
std::optional<std::vector<std::string>> ListOpenAiModels(const std::string& base_url,
                                                         const std::string& bearer,
                                                         std::chrono::milliseconds timeout,
                                                         const std::function<bool()>& cancelled,
                                                         std::string& problem);

// Why `model` is not among `served` ("not served here (it serves: a, b, …)"),
// or nullopt when it is.
std::optional<std::string> ModelNotServed(const std::string& model,
                                          const std::vector<std::string>& served);

// Why "<provider>/<model>" does not resolve through providers.json (a typo
// in the provider, a model it does not define, a backend no plugin
// provides), or nullopt when it does. Needs the running host: with no
// services it answers that it could not check.
std::optional<std::string> ModelRefProblem(const ServiceRegistry* services, const std::string& ref);

// The key a reference names, for `url` -- through the host's secrets when it
// runs, else the environment and the store directly. Why it does not
// resolve, or nullopt with `value` set ("" for no reference: keyless).
std::optional<std::string> ResolveKey(const ServiceRegistry* services, const std::string& ref,
                                      const std::string& url, std::string& value);

// Why `program` (a path, or a name on PATH) cannot be run, or nullopt.
std::optional<std::string> ExecutableProblem(const std::string& program);

}  // namespace minimal::plugins
