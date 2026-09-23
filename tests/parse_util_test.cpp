#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>
#include <stdexcept>

#include "minimal/config/parse_util.hpp"

using minimal::config::ParseStringMap;
using minimal::config::ReadClampedInt64;
using nlohmann::json;

TEST_CASE("ParseStringMap accepts a plain object of strings", "[parse_util]") {
    const auto out = ParseStringMap(json{{"A", "1"}, {"B", "2"}}, "headers");
    REQUIRE(out.size() == 2);
    CHECK(out.at("A") == "1");
    CHECK(out.at("B") == "2");
}

TEST_CASE("ParseStringMap rejects non-objects and non-string values", "[parse_util]") {
    CHECK_THROWS_AS(ParseStringMap(json::array(), "headers"), std::runtime_error);
    CHECK_THROWS_AS(ParseStringMap(json{{"A", 1}}, "headers"), std::runtime_error);
}

TEST_CASE("ParseStringMap rejects CR or LF in a KEY, not just a value", "[parse_util][invariant]") {
    // A key becomes the header NAME downstream: the value-only check left a
    // header-injection seam open through the one side of the pair that names
    // the line.
    json crlf_key = json::object();
    crlf_key["X-Injected\r\nX-Real: yes"] = "value";
    CHECK_THROWS_AS(ParseStringMap(crlf_key, "headers"), std::runtime_error);

    json cr_key = json::object();
    cr_key["X\rY"] = "value";
    CHECK_THROWS_AS(ParseStringMap(cr_key, "headers"), std::runtime_error);
}

TEST_CASE("ParseStringMap still rejects CR or LF in a value", "[parse_util]") {
    CHECK_THROWS_AS(ParseStringMap(json{{"A", "line1\nline2"}}, "headers"),
                    std::runtime_error);
}

TEST_CASE("ReadClampedInt64 falls back on absent, non-object, or non-integer fields",
          "[parse_util]") {
    CHECK(ReadClampedInt64(json{{"x", 5}}, "missing", 42, 0, 100) == 42);
    CHECK(ReadClampedInt64(json::array(), "x", 42, 0, 100) == 42);
    // Regression: is_number() (the mistake this exists to end) accepts a
    // float and get<int64_t>() silently truncates it instead of rejecting
    // it -- 250.9 must fall back, not become 250.
    CHECK(ReadClampedInt64(json{{"x", 250.9}}, "x", 42, 0, 100) == 42);
    CHECK(ReadClampedInt64(json{{"x", "5"}}, "x", 42, 0, 100) == 42);
    CHECK(ReadClampedInt64(json{{"x", true}}, "x", 42, 0, 100) == 42);
}

TEST_CASE("ReadClampedInt64 clamps into [min, max] instead of overflowing downstream",
          "[parse_util]") {
    CHECK(ReadClampedInt64(json{{"x", 50}}, "x", 0, 0, 100) == 50);
    // Regression: a value like 4294967297 used to narrow (in the plugins
    // that had their own copy of this logic) to 1 when cast to a smaller
    // int, or overflow a chrono conversion multiplied further downstream.
    // Clamping here means neither ever happens.
    CHECK(ReadClampedInt64(json{{"x", 4294967297LL}}, "x", 0, 0, 100) == 100);
    CHECK(ReadClampedInt64(json{{"x", -5}}, "x", 0, 0, 100) == 0);
}
