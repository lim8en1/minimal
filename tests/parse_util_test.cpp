#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>
#include <stdexcept>

#include "minimal/config/parse_util.hpp"

using minimal::config::ParseStringMap;
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
