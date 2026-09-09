#include <catch2/catch_test_macros.hpp>

#include "openai_response.hpp"

using namespace minimal;
using namespace minimal::provider_openai;

TEST_CASE("ParseNonStreamingResponse: plain text completion", "[openai_response]") {
    auto body = nlohmann::json::parse(R"({
        "id": "chatcmpl-1", "model": "gpt-4o-mini",
        "choices": [{"finish_reason": "stop", "message": {"role": "assistant", "content": "hello"}}]
    })");

    llm::ChatResponse response = ParseNonStreamingResponse(body);

    CHECK(response.id == "chatcmpl-1");
    CHECK(response.model == "gpt-4o-mini");
    CHECK(response.stop_reason == llm::StopReason::EndTurn);
    REQUIRE(response.content.size() == 1);
    CHECK(std::get<llm::TextBlock>(response.content[0]).text == "hello");
}

TEST_CASE("ParseNonStreamingResponse: tool-call completion", "[openai_response]") {
    auto body = nlohmann::json::parse(R"({
        "id": "chatcmpl-2", "model": "gpt-4o-mini",
        "choices": [{"finish_reason": "tool_calls", "message": {"role": "assistant", "content": null,
            "tool_calls": [{"id": "call_1", "type": "function",
                             "function": {"name": "read", "arguments": "{\"path\":\"a.txt\"}"}}]}}]
    })");

    llm::ChatResponse response = ParseNonStreamingResponse(body);

    CHECK(response.stop_reason == llm::StopReason::ToolUse);
    REQUIRE(response.content.size() == 1);
    const auto& block = std::get<llm::ToolUseBlock>(response.content[0]);
    CHECK(block.id == "call_1");
    CHECK(block.name == "read");
    CHECK(block.input == nlohmann::json{{"path", "a.txt"}});
}

TEST_CASE("ParseNonStreamingResponse: reasoning_content becomes a ThinkingBlock before the text",
          "[openai_response]") {
    // DeepSeek/GLM-style backends report reasoning on the message itself.
    // Without capture the reasoning was dropped outright in the non-streaming
    // path -- nothing persisted, nothing to show in the client.
    auto body = nlohmann::json::parse(R"({
        "id": "chatcmpl-r1", "model": "deepseek-r1",
        "choices": [{"finish_reason": "stop", "message": {"role": "assistant",
            "reasoning_content": "let me think", "content": "the answer"}}]
    })");

    llm::ChatResponse response = ParseNonStreamingResponse(body);

    REQUIRE(response.content.size() == 2);
    const auto& thinking = std::get<llm::ThinkingBlock>(response.content[0]);
    CHECK(thinking.text == "let me think");
    CHECK_FALSE(thinking.signature.has_value());
    CHECK(std::get<llm::TextBlock>(response.content[1]).text == "the answer");
}

TEST_CASE("ParseNonStreamingResponse: OpenRouter's reasoning spelling; empty and null tolerated",
          "[openai_response]") {
    auto openrouter = nlohmann::json::parse(R"({
        "id": "c", "model": "m",
        "choices": [{"finish_reason": "stop", "message": {"role": "assistant",
            "reasoning": "hmm", "content": "ok"}}]
    })");
    llm::ChatResponse response = ParseNonStreamingResponse(openrouter);
    REQUIRE(response.content.size() == 2);
    CHECK(std::get<llm::ThinkingBlock>(response.content[0]).text == "hmm");

    // Empty reasoning: no phantom block.
    auto empty = nlohmann::json::parse(R"({
        "id": "c", "model": "m",
        "choices": [{"finish_reason": "stop", "message": {"role": "assistant",
            "reasoning_content": "", "content": "ok"}}]
    })");
    response = ParseNonStreamingResponse(empty);
    REQUIRE(response.content.size() == 1);
    CHECK(std::get<llm::TextBlock>(response.content[0]).text == "ok");

    // Null reasoning: absent-equivalent, no crash.
    auto nulled = nlohmann::json::parse(R"({
        "id": "c", "model": "m",
        "choices": [{"finish_reason": "stop", "message": {"role": "assistant",
            "reasoning_content": null, "content": "ok"}}]
    })");
    response = ParseNonStreamingResponse(nulled);
    REQUIRE(response.content.size() == 1);
}

TEST_CASE("MapFinishReason", "[openai_response]") {
    CHECK(MapFinishReason("stop") == llm::StopReason::EndTurn);
    CHECK(MapFinishReason("length") == llm::StopReason::MaxTokens);
    CHECK(MapFinishReason("tool_calls") == llm::StopReason::ToolUse);
    CHECK(MapFinishReason("function_call") == llm::StopReason::ToolUse);
    CHECK(MapFinishReason("content_filter") == llm::StopReason::ContentFilter);
    CHECK(MapFinishReason("something_new") == llm::StopReason::Unknown);
}

TEST_CASE("ParseUsage: cached tokens are split out of input, Anthropic-convention", "[openai_response]") {
    // prompt_tokens INCLUDES the cached tokens (OpenAI semantics); ParseUsage
    // normalizes to the Anthropic convention (input_tokens = UNCACHED prompt)
    // that cost_model's input+cache_read pricing and the token totals assume.
    // Without the subtraction a cached turn is billed twice: full input rate
    // plus cache-read rate on the same tokens.
    auto usage_json = nlohmann::json::parse(R"({
        "prompt_tokens": 100, "completion_tokens": 20,
        "prompt_tokens_details": {"cached_tokens": 40}
    })");

    llm::Usage usage = ParseUsage(usage_json);

    CHECK(usage.input_tokens == 60);  // 100 prompt - 40 cached
    CHECK(usage.output_tokens == 20);
    CHECK(usage.cache_read_input_tokens == 40);
    CHECK(usage.cache_creation_input_tokens == 0);
}

TEST_CASE("ParseUsage: DeepSeek-style flat cache keys are honored as a fallback",
          "[openai_response]") {
    // DeepSeek's native API reports hits/misses flat instead of nested.
    const auto usage = ParseUsage(nlohmann::json::parse(
        R"({"prompt_tokens": 100, "completion_tokens": 5, "prompt_cache_hit_tokens": 30,
            "prompt_cache_miss_tokens": 70})"));
    CHECK(usage.cache_read_input_tokens == 30);
    CHECK(usage.input_tokens == 70);  // misses only

    // Nested wins when both shapes are present; the fallback only fires on a
    // zero/absent nested count.
    const auto both = ParseUsage(nlohmann::json::parse(
        R"({"prompt_tokens": 100, "completion_tokens": 5, "prompt_cache_hit_tokens": 30,
            "prompt_tokens_details": {"cached_tokens": 40}})"));
    CHECK(both.cache_read_input_tokens == 40);
    CHECK(both.input_tokens == 60);
}

TEST_CASE("ParseNonStreamingResponse: missing choices throws OpenAiParseError", "[openai_response]") {
    auto body = nlohmann::json::parse(R"({"id": "chatcmpl-3", "model": "m", "choices": []})");
    CHECK_THROWS_AS(ParseNonStreamingResponse(body), OpenAiParseError);
}

TEST_CASE("ParseNonStreamingResponse: unparsable tool-call arguments throws OpenAiParseError", "[openai_response]") {
    auto body = nlohmann::json::parse(R"({
        "id": "chatcmpl-4", "model": "m",
        "choices": [{"finish_reason": "tool_calls", "message": {"role": "assistant", "content": null,
            "tool_calls": [{"id": "call_1", "type": "function",
                             "function": {"name": "read", "arguments": "not json"}}]}}]
    })");
    CHECK_THROWS_AS(ParseNonStreamingResponse(body), OpenAiParseError);
}

TEST_CASE("finish_reason: refusal maps to StopReason::Refusal, not Unknown",
          "[openai_response][invariant]") {
    // The enum value existed, had a wire mapping and a hook name -- and was
    // produced by nothing: refusals degraded to Unknown and surfaced as a
    // generic truncation.
    const auto body = nlohmann::json::parse(
        R"({"id":"c","model":"m","choices":[{"index":0,"message":{"role":"assistant","content":null},"finish_reason":"refusal"}]})");
    const llm::ChatResponse response = ParseNonStreamingResponse(body);
    CHECK(response.stop_reason == llm::StopReason::Refusal);
}

TEST_CASE("non-streaming parse: present-but-null fields degrade, never throw",
          "[openai_response][invariant]") {
    // value(key, default) only defaults on ABSENCE: `"usage": null` threw a
    // type_error that became a non-retryable ParseError, killing the whole
    // turn. The streaming path guarded these exact fields -- the asymmetry was
    // the proof it was known.
    const auto null_usage = nlohmann::json::parse(
        R"({"id":"c","model":"m","choices":[{"index":0,"message":{"role":"assistant","content":"hi"},"finish_reason":"stop"}],"usage":null})");
    llm::ChatResponse response;
    REQUIRE_NOTHROW(response = ParseNonStreamingResponse(null_usage));
    CHECK(response.usage.input_tokens == 0);
    CHECK(response.usage.output_tokens == 0);

    const auto null_finish = nlohmann::json::parse(
        R"({"id":"c","model":"m","choices":[{"index":0,"message":{"role":"assistant","content":"hi"},"finish_reason":null}]})");
    REQUIRE_NOTHROW(response = ParseNonStreamingResponse(null_finish));
    CHECK(response.stop_reason == llm::StopReason::Unknown);

    const auto null_ids = nlohmann::json::parse(
        R"({"id":null,"model":null,"choices":[{"index":0,"message":{"role":"assistant","content":"hi"},"finish_reason":"stop"}]})");
    REQUIRE_NOTHROW(response = ParseNonStreamingResponse(null_ids));
    CHECK(response.id.empty());
}

TEST_CASE("ParseUsage: negative, fractional and absurd token counts are refused, not truncated",
          "[openai_response]") {
    const auto negative = nlohmann::json{{"prompt_tokens", -5}, {"completion_tokens", 10}};
    const llm::Usage negative_usage = ParseUsage(negative);
    CHECK(negative_usage.input_tokens == 0);  // not -5 poisoning the occupancy
    CHECK(negative_usage.output_tokens == 10);

    const auto fractional = nlohmann::json{{"prompt_tokens", 4096.7}};
    CHECK(ParseUsage(fractional).input_tokens == 0);  // the InheritNumber bug class

    const auto huge = nlohmann::json{{"completion_tokens", 9999999999999LL}};
    CHECK(ParseUsage(huge).output_tokens == std::numeric_limits<int>::max());
}
