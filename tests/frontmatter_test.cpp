#include <catch2/catch_test_macros.hpp>

#include <stdexcept>

#include "minimal/config/frontmatter.hpp"

using namespace minimal::config;

TEST_CASE("frontmatter: well-formed block plus body", "[frontmatter]") {
    auto result = parse_frontmatter_markdown(
        "---\n"
        "name: build\n"
        "provider: default\n"
        "---\n"
        "You are a build agent.\n");

    REQUIRE(result.frontmatter["name"]);
    CHECK(result.frontmatter["name"].as<std::string>() == "build");
    CHECK(result.frontmatter["provider"].as<std::string>() == "default");
    CHECK(result.body == "You are a build agent.\n");
}

TEST_CASE("frontmatter: no frontmatter block at all", "[frontmatter]") {
    // A default-constructed YAML::Node is IsDefined() == true (it's a valid Null
    // node); IsDefined() only goes false for e.g. a failed operator[] lookup. So
    // "no frontmatter block" is distinguished by IsNull(), not IsDefined().
    auto result = parse_frontmatter_markdown("Just a plain prompt body.\n");
    CHECK(result.frontmatter.IsNull());
    CHECK(result.body == "Just a plain prompt body.\n");
}

TEST_CASE("frontmatter: empty frontmatter block", "[frontmatter]") {
    auto result = parse_frontmatter_markdown("---\n---\nbody text\n");
    CHECK(result.body == "body text\n");
}

TEST_CASE("frontmatter: unterminated block throws", "[frontmatter]") {
    CHECK_THROWS_AS(parse_frontmatter_markdown("---\nname: build\nno closing delimiter\n"),
                     std::runtime_error);
}
