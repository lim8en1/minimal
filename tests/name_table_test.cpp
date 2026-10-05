// common's NameTable: one namespace's collision/alias rule, kind-agnostic. The
// server's permission-key rule built on top of it is tested with
// ResolveRegistrationNames (name_resolution_test.cpp).

#include <catch2/catch_test_macros.hpp>

#include <array>

#include "minimal/plugins/name_table.hpp"

using namespace minimal::plugins;
using Policy = NameTable::Policy;

TEST_CASE("two plugins registering the same name collide, naming both", "[name_table]") {
    NameTable table(Policy::kName);
    table.Register("tools-bash", "bash");
    table.Register("bash_over_ssh", "bash");

    const auto result = table.Resolve();
    REQUIRE(result.collisions.size() == 1);
    CHECK(result.collisions[0].surface == "bash");
    CHECK(result.collisions[0].claimants == std::vector<std::string>{"tools-bash", "bash_over_ssh"});
}

TEST_CASE("aliasing one side resolves the collision", "[name_table]") {
    NameTable table(Policy::kName);
    table.Register("tools-bash", "bash");
    table.Register("bash_over_ssh", "bash");
    table.SetAlias("bash_over_ssh.bash", "ssh_bash");

    const auto result = table.Resolve();
    CHECK(result.collisions.empty());
    REQUIRE(result.entries.size() == 2);
    CHECK(result.entries[0].surface_name == "bash");
    CHECK_FALSE(result.entries[0].aliased);
    CHECK(result.entries[1].stable_identity == "bash_over_ssh.bash");
    CHECK(result.entries[1].surface_name == "ssh_bash");
    CHECK(result.entries[1].aliased);
}

TEST_CASE("an alias naming nothing registered is dangling", "[name_table]") {
    NameTable table(Policy::kName);
    table.Register("commands-core", "compact");
    table.SetAlias("commands-core.typo", "c");
    CHECK(table.Resolve().dangling_aliases == std::vector<std::string>{"commands-core.typo"});

    NameTable empty(Policy::kName);
    empty.SetAlias("ghost-plugin.ghost_tool", "x");
    CHECK(empty.Resolve().dangling_aliases == std::vector<std::string>{"ghost-plugin.ghost_tool"});
}

TEST_CASE("a slot table is never aliased", "[name_table]") {
    NameTable table(Policy::kSlot);
    table.Register("a", "diff");
    table.Register("b", "diff");
    table.SetAlias("b.diff", "diff2");  // ignored

    const auto result = table.Resolve();
    REQUIRE(result.collisions.size() == 1);
    CHECK(result.collisions[0].surface == "diff");
    CHECK(result.dangling_aliases.empty());
}

TEST_CASE("separate tables are separate namespaces", "[name_table]") {
    NameTable tools(Policy::kName);
    NameTable services(Policy::kSlot);
    tools.Register("tools-bash", "bash");
    services.Register("tools-bash", "bash");
    CHECK(tools.Resolve().collisions.empty());
    CHECK(services.Resolve().collisions.empty());
}

TEST_CASE("a lone dotted plugin name stays a warning, not a collision", "[name_table][dotted]") {
    NameTable table(Policy::kName);
    table.Register("a.b", "widget");
    const auto result = table.Resolve();
    CHECK(result.collisions.empty());
    CHECK(result.ambiguous_identities.empty());
}

TEST_CASE("two dotted names composing the same identity are a fatal ambiguity",
          "[name_table][dotted][invariant]") {
    // ("a.b", "c") and ("a", "b.c") both compose "a.b.c": no alias or rule can
    // address one claimant without governing both.
    NameTable table(Policy::kName);
    table.Register("a.b", "c");
    table.Register("a", "b.c");
    const auto result = table.Resolve();
    CHECK(result.ambiguous_identities == std::vector<std::string>{"a.b.c"});
    CHECK(result.collisions.empty());  // distinct names: the ambiguity is the only finding
}

TEST_CASE("RouteAliases applies an alias to every name table claiming it", "[name_table]") {
    NameTable tools(Policy::kName);
    NameTable commands(Policy::kName);
    NameTable services(Policy::kSlot);
    tools.Register("p", "x");
    commands.Register("p", "x");
    services.Register("q", "svc");
    const std::array<NameTable*, 3> tables{&tools, &commands, &services};

    const auto routing = RouteAliases(
        {{"p.x", "y"}, {"q.svc", "svc2"}, {"nobody.z", "w"}}, tables);
    CHECK(routing.dangling == std::vector<std::string>{"nobody.z"});
    CHECK(routing.targets_slot == std::vector<std::string>{"q.svc"});
    CHECK(tools.Resolve().entries[0].surface_name == "y");
    CHECK(commands.Resolve().entries[0].surface_name == "y");
    CHECK(services.Resolve().entries[0].surface_name == "svc");
}
