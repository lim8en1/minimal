// The server's permission-key rule (ResolveRegistrationNames), built on
// common's NameTable: per_tool_overrides / allowed_tools reference the
// non-alias key, bare unless another registration contests the name.

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

#include "fakes.hpp"
#include "minimal/plugins/name_resolution.hpp"

using namespace minimal;
using minimal::testing::FakeTool;
using tools::ToolCategory;

namespace {

void RegisterTool(tools::ToolRegistry& registry, const std::string& plugin, const std::string& name) {
    registry.SetAttribution(plugin);
    registry.Register(std::make_unique<FakeTool>(name, ToolCategory::Execute));
    registry.ClearAttribution();
}

}  // namespace

TEST_CASE("the permission key stays bare when the original name is unambiguous",
          "[name_resolution]") {
    tools::ToolRegistry registry;
    RegisterTool(registry, "tools-bash", "bash");
    const auto names = plugins::ResolveRegistrationNames(registry, {});
    CHECK(names.permission_keys.at("bash") == "bash");
}

TEST_CASE("aliasing away one half of a collision lets the other keep its bare key",
          "[name_resolution][invariant]") {
    // Aliasing is the documented remedy for a same-name collision; it must not
    // detach an existing `"bash": ...` rule from the untouched entry. Only the
    // entry that moved away (still a distinct tool that once shared the name)
    // needs the qualified key.
    tools::ToolRegistry registry;
    RegisterTool(registry, "tools-bash", "bash");
    RegisterTool(registry, "bash_over_ssh", "bash");
    const auto names =
        plugins::ResolveRegistrationNames(registry, {{"bash_over_ssh.bash", "ssh_bash"}});
    CHECK(names.collisions.empty());
    CHECK(names.permission_keys.at("bash") == "bash");
    CHECK(names.permission_keys.at("ssh_bash") == "bash_over_ssh.bash");
}

TEST_CASE("aliasing a single, non-colliding tool keeps its bare permission key",
          "[name_resolution]") {
    // An alias for typing convenience must not detach the per-tool rule keyed
    // on the bare original name.
    tools::ToolRegistry registry;
    RegisterTool(registry, "tools-bash", "bash");
    const auto names = plugins::ResolveRegistrationNames(registry, {{"tools-bash.bash", "sh"}});
    CHECK(names.permission_keys.at("sh") == "bash");
}

TEST_CASE("an alias claimed only by a service is reported as a slot alias, not dangling",
          "[name_resolution]") {
    tools::ToolRegistry registry;
    plugins::ServiceRegistry services;
    int service = 0;
    services.SetAttribution("p");
    services.Provide("svc", &service);
    services.ClearAttribution();
    const auto names =
        plugins::ResolveRegistrationNames(registry, {{"p.svc", "x"}}, nullptr, {}, &services);
    CHECK(names.dangling_aliases.empty());
    CHECK(names.slot_aliases == std::vector<std::string>{"p.svc"});
}
