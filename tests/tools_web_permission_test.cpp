// tools-web's per-host permission model, through the real executor and the
// real web_fetch/web_search tools (nothing here touches the network: only
// the gate runs).

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <string>

#include "fakes.hpp"
#include "fetch_tool.hpp"
#include "host_permission_model.hpp"
#include "minimal/tools/permission_model.hpp"
#include "minimal/tools/tool_executor.hpp"
#include "minimal/tools/tool_registry.hpp"
#include "search_tool.hpp"
#include "url_policy.hpp"

using namespace minimal;
using minimal::tools::PermissionMode;
using minimal::tools::PermissionRunMode;

namespace {

struct Web {
    tools::ToolRegistry registry;
    tools::PermissionModelRegistry models;
    testing::UnreachablePermissionChecker checker;

    explicit Web(tools_web::WebSettings settings = {}) {
        registry.SetAttribution("tools-web");
        registry.Register(tools_web::MakeWebFetchTool(settings, net::HttpClient{}));
        registry.Register(tools_web::MakeWebSearchTool(settings, net::HttpClient{}));
        registry.ClearAttribution();
        models.SetAttribution("tools-web");
        models.Register(tools_web::MakeHostPermissionModel(settings));
        models.ClearAttribution();
    }

    // The core policy the way BuildPermissionPolicy shapes it: two layers,
    // `network` resolved into the scalar.
    static tools::PermissionPolicy Policy(std::optional<PermissionMode> preset_network = {},
                                          PermissionMode network = PermissionMode::Ask,
                                          bool trusted = true) {
        tools::PermissionOverrideLayer preset;
        preset.network = preset_network;
        tools::PermissionPolicy policy;
        policy.network = preset_network.value_or(network);
        policy.layers = std::make_shared<const tools::ToolModeLayers>(
            tools::ToolModeLayers{preset, tools::PermissionOverrideLayer{}, trusted});
        return policy;
    }

    tools::ToolExecutor::Verdict Explain(const nlohmann::json& global,
                                         const nlohmann::json& preset, const std::string& tool,
                                         const nlohmann::json& input,
                                         tools::PermissionPolicy policy = Policy(),
                                         bool trusted = true) {
        tools::ToolExecutor executor(
            registry, checker, std::move(policy),
            models.Bind({{"tools-web", global}}, {{"tools-web", preset}}, trusted),
            PermissionRunMode::Normal);
        tools::ToolContext ctx{common::DefaultLogger()};
        return executor.Explain(*registry.Find(tool), input, ctx);
    }
};

nlohmann::json Url(const std::string& url) { return {{"url", url}}; }

}  // namespace

TEST_CASE("HostOf: the host, lowercased, userinfo and brackets stripped", "[tools_web][hosts]") {
    CHECK(tools_web::HostOf("https://Docs.Example.COM/x?y") == "docs.example.com");
    CHECK(tools_web::HostOf("http://user:pw@example.com:8080/") == "example.com");
    CHECK(tools_web::HostOf("http://[::1]:80/") == "::1");
    CHECK_FALSE(tools_web::HostOf("not a url").has_value());
}

TEST_CASE("host model: no slice falls back to core's network mode", "[tools_web][hosts]") {
    Web web;
    const auto verdict = web.Explain(nullptr, nullptr, "web_fetch", Url("https://a.example/"));
    CHECK(verdict.mode == PermissionMode::Ask);
    CHECK(verdict.reason == "network access: network=ask");
}

TEST_CASE("host model: the most specific host glob wins", "[tools_web][hosts]") {
    Web web;
    const nlohmann::json global = {
        {"hosts", {{"*.example.com", "ask"}, {"api.example.com", "allow"}, {"*.internal", "deny"}}}};

    const auto api = web.Explain(global, nullptr, "web_fetch", Url("https://API.example.com/v1"));
    CHECK(api.mode == PermissionMode::AlwaysAllow);  // carved out of the broad ask
    CHECK(web.Explain(global, nullptr, "web_fetch", Url("https://www.example.com/")).mode ==
          PermissionMode::Ask);
    const auto internal = web.Explain(global, nullptr, "web_fetch", Url("http://db.internal/"));
    CHECK(internal.mode == PermissionMode::AlwaysDeny);
    CHECK(internal.reason == "network access: host db.internal matches `*.internal`=deny");
    // Unmatched: the slice's default, else core's network.
    CHECK(web.Explain({{"default", "deny"}}, nullptr, "web_fetch", Url("https://x.org/")).mode ==
          PermissionMode::AlwaysDeny);
}

TEST_CASE("host model: a preset's rule replaces the operator's for the same glob",
          "[tools_web][hosts]") {
    Web web;
    const nlohmann::json global = {{"hosts", {{"docs.example.com", "ask"}}}};
    const nlohmann::json preset = {{"hosts", {{"docs.example.com", "allow"}}}};
    CHECK(web.Explain(global, preset, "web_fetch", Url("https://docs.example.com/")).mode ==
          PermissionMode::AlwaysAllow);
}

TEST_CASE("host model: an untrusted preset only narrows", "[tools_web][hosts]") {
    Web web;
    const nlohmann::json preset = {{"hosts", {{"*", "allow"}}}};
    CHECK(web.Explain(nullptr, preset, "web_fetch", Url("https://x.org/"),
                      Web::Policy({}, PermissionMode::Ask, false), /*trusted=*/false)
              .mode == PermissionMode::Ask);
    const nlohmann::json narrowing = {{"hosts", {{"x.org", "deny"}}}};
    CHECK(web.Explain({{"default", "allow"}}, narrowing, "web_fetch", Url("https://x.org/"),
                      Web::Policy({}, PermissionMode::Ask, false), /*trusted=*/false)
              .mode == PermissionMode::AlwaysDeny);
}

TEST_CASE("host model: a preset's own network setting beats the operator's host allow",
          "[tools_web][hosts]") {
    // plan-style `network: deny` in the preset: the operator's global host
    // allow must not widen it; only the preset's own host rules may.
    Web web;
    const nlohmann::json global = {{"hosts", {{"docs.example.com", "allow"}}}};
    const auto denied = web.Explain(global, nullptr, "web_fetch", Url("https://docs.example.com/"),
                                    Web::Policy(PermissionMode::AlwaysDeny));
    CHECK(denied.mode == PermissionMode::AlwaysDeny);
    CHECK(denied.reason == "network access: preset: network=deny");

    const nlohmann::json preset = {{"hosts", {{"docs.example.com", "allow"}}}};
    CHECK(web.Explain(global, preset, "web_fetch", Url("https://docs.example.com/"),
                      Web::Policy(PermissionMode::AlwaysDeny))
              .mode == PermissionMode::AlwaysAllow);
}

TEST_CASE("host model: web_search is judged by its backend's host", "[tools_web][hosts]") {
    tools_web::WebSettings settings;
    settings.search_api_url = "https://search.local.example/api";
    Web web(settings);
    const nlohmann::json global = {{"hosts", {{"search.local.example", "allow"}}}};
    CHECK(web.Explain(global, nullptr, "web_search", {{"query", "anything"}}).mode ==
          PermissionMode::AlwaysAllow);
}

TEST_CASE("host model: the parent's host rules are a ceiling", "[tools_web][hosts]") {
    Web web;
    tools::ToolExecutor executor(
        web.registry, web.checker, Web::Policy(),
        web.models.Bind({{"tools-web", {{"default", "allow"}}}}, nullptr, true),
        PermissionRunMode::Normal, Web::Policy(),
        web.models.Bind({{"tools-web", {{"hosts", {{"x.org", "deny"}}}}}}, nullptr, true));
    tools::ToolContext ctx{common::DefaultLogger()};
    const auto verdict = executor.Explain(*web.registry.Find("web_fetch"), Url("https://x.org/"), ctx);
    CHECK(verdict.mode == PermissionMode::AlwaysDeny);
    CHECK(verdict.reason.rfind("parent session, network access:", 0) == 0);
}

TEST_CASE("host model: claims only its own tools; a bad slice names itself",
          "[tools_web][hosts]") {
    Web web;
    CHECK(web.models.ValidateClaims(web.registry).empty());
    try {
        web.models.Bind({{"tools-web", {{"hosts", {{"x", "maybe"}}}}}}, nullptr, true);
        FAIL("expected a throw");
    } catch (const std::exception& e) {
        CHECK(std::string(e.what()) ==
              "permission model 'tools-web': plugins.tools-web.hosts[\"x\"] must be "
              "\"allow\", \"ask\" or \"deny\"");
    }
    CHECK_THROWS(web.models.Bind({{"tools-web", {{"hostz", {}}}}}, nullptr, true));
}
