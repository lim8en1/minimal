#include <catch2/catch_test_macros.hpp>

#include "openai_error_mapping.hpp"

using namespace minimal;
using namespace minimal::provider_openai;

TEST_CASE("ClassifyHttpFailure: 429 with Retry-After", "[openai_error_mapping]") {
    llm::LlmError error = ClassifyHttpFailure(429, R"({"error":{"message":"slow down"}})", {{"Retry-After", "30"}});

    CHECK(error.category() == llm::LlmErrorCategory::RateLimit);
    REQUIRE(error.retry_after().has_value());
    CHECK(error.retry_after()->count() == 30000);
    CHECK(std::string(error.what()) == "slow down");
}

TEST_CASE("ClassifyHttpFailure: 429 without Retry-After", "[openai_error_mapping]") {
    llm::LlmError error = ClassifyHttpFailure(429, "{}", {});
    CHECK(error.category() == llm::LlmErrorCategory::RateLimit);
    CHECK_FALSE(error.retry_after().has_value());
}

TEST_CASE("ClassifyHttpFailure: 401 and 403 are AuthError", "[openai_error_mapping]") {
    CHECK(ClassifyHttpFailure(401, "{}", {}).category() == llm::LlmErrorCategory::AuthError);
    CHECK(ClassifyHttpFailure(403, "{}", {}).category() == llm::LlmErrorCategory::AuthError);
}

TEST_CASE("ClassifyHttpFailure: 5xx is ServerError", "[openai_error_mapping]") {
    CHECK(ClassifyHttpFailure(500, "{}", {}).category() == llm::LlmErrorCategory::ServerError);
    CHECK(ClassifyHttpFailure(503, "{}", {}).category() == llm::LlmErrorCategory::ServerError);
}

TEST_CASE("ClassifyHttpFailure: other 4xx is RequestError", "[openai_error_mapping]") {
    CHECK(ClassifyHttpFailure(400, "{}", {}).category() == llm::LlmErrorCategory::RequestError);
    CHECK(ClassifyHttpFailure(404, "{}", {}).category() == llm::LlmErrorCategory::RequestError);
    CHECK(ClassifyHttpFailure(422, "{}", {}).category() == llm::LlmErrorCategory::RequestError);
}

TEST_CASE("ClassifyHttpFailure: falls back to a generic message when body isn't the expected shape",
          "[openai_error_mapping]") {
    llm::LlmError error = ClassifyHttpFailure(500, "not json at all", {});
    CHECK(std::string(error.what()) == "HTTP 500");
}

TEST_CASE("ClassifyTransportFailure: every non-Cancelled kind maps to Transport", "[openai_error_mapping]") {
    CHECK(ClassifyTransportFailure(llm::HttpError(llm::HttpErrorKind::ConnectFailed, "x")).category() ==
          llm::LlmErrorCategory::Transport);
    CHECK(ClassifyTransportFailure(llm::HttpError(llm::HttpErrorKind::TlsError, "x")).category() ==
          llm::LlmErrorCategory::Transport);
    CHECK(ClassifyTransportFailure(llm::HttpError(llm::HttpErrorKind::Timeout, "x")).category() ==
          llm::LlmErrorCategory::Transport);
    CHECK(ClassifyTransportFailure(llm::HttpError(llm::HttpErrorKind::Other, "x")).category() ==
          llm::LlmErrorCategory::Transport);
}
