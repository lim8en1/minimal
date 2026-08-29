#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include "minimal/plugins/name_registry.hpp"

using namespace minimal::plugins;

namespace {
bool Contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}
}  // namespace

TEST_CASE("two plugins registering the same tool name collide", "[name_registry]") {
    NameRegistry reg;
    reg.Register(NameKind::Tool, "tools-bash", "bash");
    reg.Register(NameKind::Tool, "bash_over_ssh", "bash");

    auto result = reg.Resolve();
    REQUIRE(result.fatal_collisions.size() == 1);
    CHECK(result.fatal_collisions[0] == "bash");
}

TEST_CASE("aliasing one side resolves the collision", "[name_registry]") {
    NameRegistry reg;
    reg.Register(NameKind::Tool, "tools-bash", "bash");
    reg.Register(NameKind::Tool, "bash_over_ssh", "bash");
    reg.SetAlias(NameKind::Tool, "bash_over_ssh.bash", "ssh_bash");

    auto result = reg.Resolve();
    CHECK(result.fatal_collisions.empty());

    bool found_core = false, found_ssh = false;
    for (const auto& e : result.entries) {
        if (e.stable_identity == "tools-bash.bash") {
            CHECK(e.surface_name == "bash");
            found_core = true;
        }
        if (e.stable_identity == "bash_over_ssh.bash") {
            CHECK(e.surface_name == "ssh_bash");
            found_ssh = true;
        }
    }
    CHECK(found_core);
    CHECK(found_ssh);
}

TEST_CASE("an alias naming nothing registered is dangling", "[name_registry]") {
    NameRegistry reg;
    reg.Register(NameKind::Command, "commands-core", "compact");
    reg.SetAlias(NameKind::Command, "commands-core.typo", "c");

    auto result = reg.Resolve();
    CHECK(Contains(result.dangling_aliases, "commands-core.typo"));
}

TEST_CASE("permission key stays bare when the original name is unambiguous", "[name_registry]") {
    NameRegistry reg;
    reg.Register(NameKind::Tool, "tools-bash", "bash");

    auto result = reg.Resolve();
    REQUIRE(result.entries.size() == 1);
    CHECK(result.entries[0].permission_key == "bash");
}

TEST_CASE("permission key qualifies when the original name is ambiguous, even after aliasing",
          "[name_registry]") {
    NameRegistry reg;
    reg.Register(NameKind::Tool, "tools-bash", "bash");
    reg.Register(NameKind::Tool, "bash_over_ssh", "bash");
    reg.SetAlias(NameKind::Tool, "bash_over_ssh.bash", "ssh_bash");

    auto result = reg.Resolve();
    for (const auto& e : result.entries) {
        CHECK(e.permission_key == e.stable_identity);
    }
}
