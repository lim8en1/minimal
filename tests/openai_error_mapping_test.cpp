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
    CHECK(ClassifyTransportFailure(net::HttpError(net::HttpErrorKind::ConnectFailed, "x")).category() ==
          llm::LlmErrorCategory::Transport);
    CHECK(ClassifyTransportFailure(net::HttpError(net::HttpErrorKind::TlsError, "x")).category() ==
          llm::LlmErrorCategory::Transport);
    CHECK(ClassifyTransportFailure(net::HttpError(net::HttpErrorKind::Timeout, "x")).category() ==
          llm::LlmErrorCategory::Transport);
    CHECK(ClassifyTransportFailure(net::HttpError(net::HttpErrorKind::Other, "x")).category() ==
          llm::LlmErrorCategory::Transport);
}

TEST_CASE("ClassifyHttpFailure: 408 is retryable, not a dead-end RequestError",
          "[openai_error_mapping][invariant]") {
    // HTTP 408 is the HTTP analogue of the transport timeouts the loop
    // retries; Azure-fronted gateways emit it routinely. Under RequestError it
    // failed a whole turn on one transient gateway timeout.
    llm::LlmError error = ClassifyHttpFailure(408, "{}", {});
    CHECK(error.category() == llm::LlmErrorCategory::Transport);
    CHECK(error.is_retryable());
}

TEST_CASE("ClassifyHttpFailure: Retry-After in both wire forms, on any status",
          "[openai_error_mapping]") {
    // Seconds form, on a 429 (the shape that always worked).
    llm::LlmError seconds = ClassifyHttpFailure(429, "{}", {{"Retry-After", "30"}});
    REQUIRE(seconds.retry_after().has_value());
    CHECK(*seconds.retry_after() == std::chrono::milliseconds{30000});

    // Milliseconds form -- what OpenAI's own 429s carry. Silently ignored
    // before; the only consumer of retry_after() now honors it.
    llm::LlmError ms = ClassifyHttpFailure(429, "{}", {{"retry-after-ms", "2500"}});
    REQUIRE(ms.retry_after().has_value());
    CHECK(*ms.retry_after() == std::chrono::milliseconds{2500});

    // Any status: OpenRouter/vLLM attach it to 503/529 overloads.
    llm::LlmError overload = ClassifyHttpFailure(503, "{}", {{"Retry-After", "5"}});
    CHECK(overload.category() == llm::LlmErrorCategory::ServerError);
    REQUIRE(overload.retry_after().has_value());
    CHECK(*overload.retry_after() == std::chrono::milliseconds{5000});
}

TEST_CASE("ClassifyHttpFailure: hostile Retry-After values leave the hint unset",
          "[openai_error_mapping]") {
    CHECK_FALSE(ClassifyHttpFailure(429, "{}", {{"Retry-After", "-5"}}).retry_after().has_value());
    CHECK_FALSE(ClassifyHttpFailure(429, "{}", {{"Retry-After", "99999999999999999999"}})
                    .retry_after()
                    .has_value());
    CHECK_FALSE(
        ClassifyHttpFailure(429, "{}", {{"Retry-After", "http://date"}}).retry_after().has_value());
    // Trailing garbage is not a value the server stated.
    CHECK_FALSE(ClassifyHttpFailure(429, "{}", {{"retry-after-ms", "12x"}}).retry_after().has_value());

    // "9999999999999999" (seconds) is well within std::stoll's range (unlike
    // the "99999999999999999999" case above, which throws out_of_range and
    // never reaches the multiply) but overflows the SIGNED 64-bit `parsed *
    // 1000LL` -- UB -- if that multiply runs before the sanity bound is
    // checked. The bound must reject it on `parsed` itself, pre-multiply.
    CHECK_FALSE(ClassifyHttpFailure(429, "{}", {{"Retry-After", "9999999999999999"}})
                    .retry_after()
                    .has_value());
}

TEST_CASE("ClassifyErrorEventData: an event:error frame becomes a classified error",
          "[openai_error_mapping][invariant]") {
    // These frames are how gateways deliver mid-stream failures; they used to
    // be silently swallowed and the turn came back EMPTY, presented as
    // success. The classification is ServerError (retryable -- a gateway
    // blip) unless the message names an auth failure (a bad key must not
    // hammer the backend with retries).
    llm::LlmError server =
        ClassifyErrorEventData(R"({"error":{"message":"upstream overloaded"}})");
    CHECK(server.category() == llm::LlmErrorCategory::ServerError);
    CHECK(std::string(server.what()) == "upstream overloaded");

    llm::LlmError auth =
        ClassifyErrorEventData(R"({"error":{"message":"invalid api key"}})");
    CHECK(auth.category() == llm::LlmErrorCategory::AuthError);
    CHECK_FALSE(auth.is_retryable());

    llm::LlmError raw = ClassifyErrorEventData("gateway exploded");
    CHECK(std::string(raw.what()) == "gateway exploded");
}
