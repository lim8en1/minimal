#include <catch2/catch_test_macros.hpp>

#include "minimal/config/permission_codec.hpp"

using namespace minimal::config;
using minimal::tools::PermissionMode;
using minimal::tools::PermissionRunMode;

TEST_CASE("ParsePermissionMode round-trips all three values", "[permission_codec]") {
    CHECK(ParsePermissionMode("allow") == PermissionMode::AlwaysAllow);
    CHECK(ParsePermissionMode("ask") == PermissionMode::Ask);
    CHECK(ParsePermissionMode("deny") == PermissionMode::AlwaysDeny);
}

TEST_CASE("ParsePermissionMode throws on an unknown string", "[permission_codec]") {
    CHECK_THROWS_AS(ParsePermissionMode("maybe"), std::runtime_error);
    CHECK_THROWS_AS(ParsePermissionMode(""), std::runtime_error);
}

TEST_CASE("ParsePermissionRunMode round-trips all three values", "[permission_codec]") {
    CHECK(ParsePermissionRunMode("normal") == PermissionRunMode::Normal);
    CHECK(ParsePermissionRunMode("bypass") == PermissionRunMode::Bypass);
    CHECK(ParsePermissionRunMode("allow-only") == PermissionRunMode::AllowOnly);
}

TEST_CASE("ParsePermissionRunMode throws on an unknown string", "[permission_codec]") {
    CHECK_THROWS_AS(ParsePermissionRunMode("sometimes"), std::runtime_error);
}
