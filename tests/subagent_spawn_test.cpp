#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "minimal/agent/subagent_spawn.hpp"

using minimal::agent::intersect_allowed;

namespace {

std::vector<std::string> Intersect(std::vector<std::string> parent,
                                   std::vector<std::string> child) {
    return intersect_allowed(parent, child);
}

}  // namespace

TEST_CASE("intersect_allowed keeps only names present in both", "[subagent]") {
    CHECK(Intersect({"read", "bash", "edit"}, {"bash", "write"}) ==
          std::vector<std::string>{"bash"});
}

TEST_CASE("intersect_allowed preserves the child's order and drops duplicates", "[subagent]") {
    CHECK(Intersect({"a", "b", "c"}, {"c", "b", "c", "b"}) ==
          std::vector<std::string>{"c", "b"});
}

TEST_CASE("a star on either side is identity for the other", "[subagent]") {
    // The parent allows everything: the child's own list is the bound.
    CHECK(Intersect({"*"}, {"bash", "read"}) == std::vector<std::string>{"bash", "read"});
    // The child asks for everything: the parent's list is the bound. A child's
    // "*" must NOT widen what the parent permits.
    CHECK(Intersect({"bash", "read"}, {"*"}) == std::vector<std::string>{"bash", "read"});
    CHECK(Intersect({"*"}, {"*"}) == std::vector<std::string>{"*"});
}

TEST_CASE("an empty parent yields an empty intersection whatever the child asks",
          "[subagent]") {
    CHECK(Intersect({}, {"bash"}).empty());
    CHECK(Intersect({}, {"*"}).empty());
}

TEST_CASE("disjoint lists yield an empty intersection", "[subagent]") {
    CHECK(Intersect({"read"}, {"bash"}).empty());
}
