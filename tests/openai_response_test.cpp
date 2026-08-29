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

TEST_CASE("MapFinishReason", "[openai_response]") {
    CHECK(MapFinishReason("stop") == llm::StopReason::EndTurn);
    CHECK(MapFinishReason("length") == llm::StopReason::MaxTokens);
    CHECK(MapFinishReason("tool_calls") == llm::StopReason::ToolUse);
    CHECK(MapFinishReason("function_call") == llm::StopReason::ToolUse);
    CHECK(MapFinishReason("content_filter") == llm::StopReason::ContentFilter);
    CHECK(MapFinishReason("something_new") == llm::StopReason::Unknown);
}

TEST_CASE("ParseUsage: including cached tokens", "[openai_response]") {
    auto usage_json = nlohmann::json::parse(R"({
        "prompt_tokens": 100, "completion_tokens": 20,
        "prompt_tokens_details": {"cached_tokens": 40}
    })");

    llm::Usage usage = ParseUsage(usage_json);

    CHECK(usage.input_tokens == 100);
    CHECK(usage.output_tokens == 20);
    CHECK(usage.cache_read_input_tokens == 40);
    CHECK(usage.cache_creation_input_tokens == 0);
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
