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
