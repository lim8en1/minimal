#include <string>

#include <catch2/catch_test_macros.hpp>

#include "minimal/llm/json_depth.hpp"

using minimal::llm::JsonDepthWithinLimit;

TEST_CASE("JsonDepthWithinLimit accepts ordinary shallow payloads", "[json_depth]") {
    CHECK(JsonDepthWithinLimit(R"({"a": 1, "b": [1, 2, 3]})"));
    CHECK(JsonDepthWithinLimit("{}"));
    CHECK(JsonDepthWithinLimit(""));
}

TEST_CASE("JsonDepthWithinLimit rejects genuinely deep nesting", "[json_depth]") {
    const std::string deep(600, '[');
    CHECK_FALSE(JsonDepthWithinLimit(deep, 512));
    const std::string shallow_enough(500, '[');
    CHECK(JsonDepthWithinLimit(shallow_enough, 512));
}

TEST_CASE(
    "JsonDepthWithinLimit accepts a wide-but-shallow payload, not just a total open count",
    "[json_depth][regression]") {
    // The bug: the guard counted the cumulative total of every '{'/'[' seen
    // in the whole document, never decrementing on a matching close -- so it
    // measured total container count, not actual nesting depth. A flat array
    // of 600 sibling objects never nests past depth 2, but a "total opens"
    // counter reaches 601 (the array plus each object), rejecting a
    // perfectly ordinary payload as if it were a stack-overflow attempt.
    std::string wide = "[";
    for (int i = 0; i < 600; ++i) wide += "{},";
    wide += "{}]";
    CHECK(JsonDepthWithinLimit(wide, 512));
}

TEST_CASE("JsonDepthWithinLimit ignores braces inside string literals", "[json_depth]") {
    CHECK(JsonDepthWithinLimit(R"({"text": "[[[[[[[[[[[[[[["})"));
    CHECK(JsonDepthWithinLimit(R"({"text": "escaped \" quote then [ stays a string"})"));
}

TEST_CASE("JsonDepthWithinLimit tolerates an unbalanced close without underflowing",
          "[json_depth][invariant]") {
    // This is a resource guard, not a validator (parse() catches malformed
    // documents) -- an extra ']'/'}' must not underflow the depth counter
    // and must not itself be mistaken for danger.
    CHECK(JsonDepthWithinLimit("]]]]]]]]]]"));
    CHECK(JsonDepthWithinLimit("{}}}}}}}}}"));
}
