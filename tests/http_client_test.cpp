#include <catch2/catch_test_macros.hpp>

#include "minimal/llm/http_client.hpp"

using namespace minimal::llm;

TEST_CASE("FindHeaderCI matches regardless of case", "[http_client]") {
    std::vector<HttpHeader> headers{{"Content-Type", "application/json"}, {"Retry-After", "30"}};

    CHECK(FindHeaderCI(headers, "content-type") == "application/json");
    CHECK(FindHeaderCI(headers, "CONTENT-TYPE") == "application/json");
    CHECK(FindHeaderCI(headers, "retry-after") == "30");
}

TEST_CASE("FindHeaderCI returns nullopt when absent", "[http_client]") {
    std::vector<HttpHeader> headers{{"Content-Type", "application/json"}};
    CHECK_FALSE(FindHeaderCI(headers, "Authorization").has_value());
    CHECK_FALSE(FindHeaderCI({}, "Content-Type").has_value());
}
