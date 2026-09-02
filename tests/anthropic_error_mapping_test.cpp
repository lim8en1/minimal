#include <catch2/catch_test_macros.hpp>

#include "anthropic_error_mapping.hpp"

using namespace minimal;
using namespace minimal::provider_anthropic;

TEST_CASE("ClassifyHttpFailure: classifies by the body's own error.type", "[anthropic_error]") {
    const auto error = ClassifyHttpFailure(
        529, R"({"type":"error","error":{"type":"overloaded_error","message":"Overloaded"}})", {});
    CHECK(error.category() == llm::LlmErrorCategory::ServerError);
    CHECK(std::string(error.what()) == "Overloaded");
    CHECK(error.http_status() == 529);
}

TEST_CASE("ClassifyHttpFailure: rate_limit_error is retryable", "[anthropic_error]") {
    const auto error = ClassifyHttpFailure(
        429, R"({"type":"error","error":{"type":"rate_limit_error","message":"slow down"}})", {});
    CHECK(error.category() == llm::LlmErrorCategory::RateLimit);
    CHECK(error.is_retryable());
}

TEST_CASE("ClassifyHttpFailure: authentication_error is not retryable", "[anthropic_error]") {
    const auto error = ClassifyHttpFailure(
        401, R"({"type":"error","error":{"type":"authentication_error","message":"bad key"}})",
        {});
    CHECK(error.category() == llm::LlmErrorCategory::AuthError);
    CHECK_FALSE(error.is_retryable());
}

TEST_CASE("ClassifyHttpFailure: invalid_request_error is not retryable", "[anthropic_error]") {
    const auto error = ClassifyHttpFailure(
        400,
        R"({"type":"error","error":{"type":"invalid_request_error","message":"bad shape"}})", {});
    CHECK(error.category() == llm::LlmErrorCategory::RequestError);
    CHECK_FALSE(error.is_retryable());
}

TEST_CASE("ClassifyHttpFailure: falls back to status-code classification when the body isn't "
         "the expected JSON shape",
         "[anthropic_error]") {
    const auto rate_limited = ClassifyHttpFailure(429, "not json", {});
    CHECK(rate_limited.category() == llm::LlmErrorCategory::RateLimit);

    const auto server_error = ClassifyHttpFailure(503, "{}", {});
    CHECK(server_error.category() == llm::LlmErrorCategory::ServerError);

    const auto auth_error = ClassifyHttpFailure(403, "", {});
    CHECK(auth_error.category() == llm::LlmErrorCategory::AuthError);
}

TEST_CASE("ClassifyHttpFailure: Retry-After header, seconds only", "[anthropic_error]") {
    const auto error = ClassifyHttpFailure(
        429, R"({"type":"error","error":{"type":"rate_limit_error","message":"slow down"}})",
        {{"Retry-After", "30"}});
    REQUIRE(error.retry_after().has_value());
    CHECK(error.retry_after()->count() == 30000);
}

TEST_CASE("ClassifyHttpFailure: a hostile Retry-After value leaves the hint unset",
         "[anthropic_error]") {
    const auto error = ClassifyHttpFailure(429, "{}", {{"Retry-After", "not-a-number"}});
    CHECK_FALSE(error.retry_after().has_value());

    const auto negative = ClassifyHttpFailure(429, "{}", {{"Retry-After", "-5"}});
    CHECK_FALSE(negative.retry_after().has_value());
}

TEST_CASE("ClassifyErrorEventData: an event: error SSE frame", "[anthropic_error]") {
    const auto error = ClassifyErrorEventData(
        R"({"type":"error","error":{"type":"overloaded_error","message":"Overloaded"}})");
    CHECK(error.category() == llm::LlmErrorCategory::ServerError);
    CHECK(std::string(error.what()) == "Overloaded");
}

TEST_CASE("ClassifyErrorEventData: non-JSON data falls back to ServerError with the raw text",
         "[anthropic_error]") {
    const auto error = ClassifyErrorEventData("something went wrong");
    CHECK(error.category() == llm::LlmErrorCategory::ServerError);
    CHECK(std::string(error.what()) == "something went wrong");
}

TEST_CASE("ClassifyTransportFailure: always Transport", "[anthropic_error]") {
    const llm::HttpError http_error(llm::HttpErrorKind::ConnectFailed, "connection refused");
    const auto error = ClassifyTransportFailure(http_error);
    CHECK(error.category() == llm::LlmErrorCategory::Transport);
}
