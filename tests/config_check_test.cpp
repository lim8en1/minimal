// The config-check probes (minimal/plugins/config_check.hpp): each answers
// "fine" or a sentence a user can act on.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <string>
#include <vector>

#include "loopback_http_server.hpp"
#include "minimal/plugins/config_check.hpp"

using namespace minimal;
using Catch::Matchers::ContainsSubstring;
using namespace std::chrono_literals;

namespace {

std::string Response(int status, const std::string& body, const std::string& type = "application/json") {
    return "HTTP/1.1 " + std::to_string(status) + " X\r\nContent-Type: " + type +
           "\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
}

const auto kNever = [] { return false; };

}  // namespace

TEST_CASE("an HTTP probe says what went wrong in words", "[config_check]") {
    {
        test::LoopbackHttpServer server({Response(200, R"({"data": []})")});
        const auto probe = plugins::ProbeHttp(server.url("/v1/x"), "k", 2s, kNever);
        CHECK(probe.ok);
        CHECK(probe.body.contains("data"));
        CHECK_THAT(server.requests().at(0), ContainsSubstring("Authorization: Bearer k"));
    }
    {
        test::LoopbackHttpServer server({Response(401, R"({"error": {"message": "bad key"}})")});
        const auto probe = plugins::ProbeHttp(server.url("/"), "", 2s, kNever);
        CHECK_FALSE(probe.ok);
        CHECK_THAT(probe.problem, ContainsSubstring("the key was refused"));
        CHECK_THAT(probe.problem, ContainsSubstring("bad key"));
    }
    {
        test::LoopbackHttpServer server({Response(404, "no")});
        CHECK_THAT(plugins::ProbeHttp(server.url("/typo"), "", 2s, kNever).problem,
                   ContainsSubstring("a typo in the path"));
    }
    {
        test::LoopbackHttpServer server({Response(200, "<html>", "text/html")});
        CHECK_THAT(plugins::ProbeHttp(server.url("/"), "", 2s, kNever).problem,
                   ContainsSubstring("not as an API"));
    }
    CHECK_THAT(plugins::ProbeHttp("http://127.0.0.1:1/", "", 2s, kNever).problem,
               ContainsSubstring("cannot connect to 127.0.0.1:1"));
    CHECK_THAT(plugins::ProbeHttp("ftp://x", "", 2s, kNever).problem,
               ContainsSubstring("not an http(s) URL"));
}

TEST_CASE("an OpenAI-shaped model list, and a model it does not serve", "[config_check]") {
    test::LoopbackHttpServer server(
        {Response(200, R"({"data": [{"id": "alpha"}, {"id": "beta"}]})"), Response(200, "{}")});
    std::string problem;
    const auto ids = plugins::ListOpenAiModels(server.url("/v1/"), "", 2s, kNever, problem);
    REQUIRE(ids.has_value());
    CHECK(*ids == std::vector<std::string>{"alpha", "beta"});
    CHECK_THAT(server.requests().at(0), ContainsSubstring("GET /v1/models"));
    CHECK_FALSE(plugins::ModelNotServed("alpha", *ids).has_value());
    CHECK(plugins::ModelNotServed("alpah", *ids) == "not served here (it serves: alpha, beta)");
    CHECK_FALSE(plugins::ListOpenAiModels(server.url("/v1"), "", 2s, kNever, problem).has_value());
    CHECK_THAT(problem, ContainsSubstring("not with a model list"));
}

TEST_CASE("model refs need the running host; programs are found or not", "[config_check]") {
    CHECK(plugins::ModelRefProblem(nullptr, "local/x") == "not checked: the plugin is not running");
    CHECK_FALSE(plugins::ExecutableProblem("sh").has_value());
    CHECK_FALSE(plugins::ExecutableProblem("/bin/sh").has_value());
    CHECK(plugins::ExecutableProblem("no-such-program-xyz") == "no-such-program-xyz is not on PATH");
    CHECK(plugins::ExecutableProblem("/no/such/python") == "no such file: /no/such/python");
    std::string key;
    CHECK_FALSE(plugins::ResolveKey(nullptr, "", "https://x", key).has_value());
    CHECK(key.empty());
    CHECK(plugins::ResolveKey(nullptr, "env:MINIMAL_SURELY_UNSET_VAR_42", "https://x", key)
              .has_value());
}

// --- the bundled plugins' checks, as the endpoint calls them ---------------------

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "minimal/plugins/plugin_loader.hpp"
#include "temp_dir.hpp"

#ifndef MINIMAL_BUNDLED_SERVER_PLUGIN_DIR
#error "MINIMAL_BUNDLED_SERVER_PLUGIN_DIR must name the built server plugins"
#endif

namespace {

// The built bundled plugin's check symbol.
decltype(plugins::PluginConfigSymbols::check) CheckOf(const std::string& stem) {
    for (const auto& file :
         std::filesystem::recursive_directory_iterator(MINIMAL_BUNDLED_SERVER_PLUGIN_DIR)) {
        if (file.path().filename() != stem + ".so") continue;
        const auto inspected = plugins::InspectPluginConfig(file.path());
        REQUIRE(inspected.error.empty());
        return inspected.config.check;
    }
    FAIL("no built plugin " + stem);
    return nullptr;
}

nlohmann::json Run(const std::string& stem, const nlohmann::json& config,
                   std::chrono::milliseconds budget = 5s) {
    const auto check = CheckOf(stem);
    REQUIRE(check != nullptr);
    plugins::ConfigCheckContext context;
    context.budget = budget;
    nlohmann::json out;
    check(config, context, out);
    return out;
}

// The finding for `path`, or null.
nlohmann::json At(const nlohmann::json& results, const std::string& path) {
    for (const auto& row : results) {
        if (row.value("path", "") == path) return row;
    }
    return nullptr;
}

}  // namespace

TEST_CASE("bundled checks: the shell and the interpreter run, or say why not", "[config_check]") {
    CHECK(At(Run("tools-bash", nlohmann::json::object()), "shell").at("level") == "ok");
    const auto bad = At(Run("tools-bash", {{"shell", "/no/such/sh"}}), "shell");
    CHECK(bad.at("level") == "error");
    CHECK(bad.at("message") == "no such file: /no/such/sh");
    CHECK(At(Run("tools-python", {{"python", "no-such-python-xyz"}}), "python").at("message") ==
          "no-such-python-xyz is not on PATH");
}

TEST_CASE("bundled checks: each rate source fetched, its finding on its own entry",
          "[config_check]") {
    const std::string litellm =
        R"({"m": {"input_cost_per_token": 0.000001, "output_cost_per_token": 0.000002}})";
    test::LoopbackHttpServer server(
        {Response(200, litellm), Response(404, "no"), Response(200, R"({"data": []})")});
    const auto results = Run("cost-tracker",
                             {{"rate_sources",
                               {{{"format", "litellm"}, {"url", server.url("/prices")}},
                                {{"format", "litellm"}, {"name", "b"}, {"url", server.url("/typo")}},
                                {{"format", "litellm"}, {"name", "c"}, {"url", server.url("/other")}}}}});
    CHECK(At(results, "rate_sources[0].url").at("message") == "1 models priced");
    CHECK_THAT(At(results, "rate_sources[1].url").at("message").get<std::string>(),
               ContainsSubstring("a typo in the path"));
    CHECK_THAT(At(results, "rate_sources[2].url").at("message").get<std::string>(),
               ContainsSubstring("holds no prices in the litellm shape"));
}

TEST_CASE("bundled checks: memory names a model its embedding server does not serve",
          "[config_check]") {
    test::LoopbackHttpServer server(
        {Response(200, R"({"data": [{"id": "embed-a"}, {"id": "embed-c"}]})"),
         Response(400, R"({"error": {"message": "model not found"}})")});
    const auto results = Run("memory", {{"strategy", "http"},
                                        {"api_base", server.url("/v1")},
                                        {"model", "embed-b"}});
    CHECK(At(results, "model").at("message") == "not served here (it serves: embed-a, embed-c)");
    CHECK_THAT(At(results, "api_base").at("message").get<std::string>(),
               ContainsSubstring("model not found"));
}

TEST_CASE("bundled checks: a model ref is not checked without the running host",
          "[config_check]") {
    const auto row = At(Run("session-titler", {{"model", "local/x"}}), "model");
    CHECK(row.at("level") == "warning");
    CHECK(row.at("message") == "not checked: the plugin is not running");
    CHECK(At(Run("session-titler", nlohmann::json::object()), "model").at("level") == "ok");
}

TEST_CASE("bundled checks: the advisor puts a refused key on api_key", "[config_check]") {
    minimal::testing::TempDir config_dir;
    std::filesystem::copy_file(std::filesystem::path(MINIMAL_BUNDLED_DIR) /
                                   "decision-advisor.json",
                               config_dir.path() / "decision-advisor.json");
    const char* previous = std::getenv("MINIMAL_CONFIG_DIR");
    const std::string saved = previous != nullptr ? previous : "";
    ::setenv("MINIMAL_CONFIG_DIR", config_dir.path().c_str(), 1);
    test::LoopbackHttpServer server({Response(401, R"({"error": "invalid key"})")});
    const auto results = Run("decision-advisor", {{"backend", "llama"},
                                                  {"endpoint", server.url("/v1/systemone")}});
    if (previous != nullptr) {
        ::setenv("MINIMAL_CONFIG_DIR", saved.c_str(), 1);
    } else {
        ::unsetenv("MINIMAL_CONFIG_DIR");
    }
    INFO(results.dump());
    REQUIRE(At(results, "api_key").is_object());
    CHECK(At(results, "api_key").at("message") == "the backend refused the key (HTTP 401 (invalid key))");
}
