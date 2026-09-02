#include <catch2/catch_test_macros.hpp>

#include "anthropic_response.hpp"

using namespace minimal;
using namespace minimal::provider_anthropic;

TEST_CASE("MapStopReason", "[anthropic_response]") {
    CHECK(MapStopReason("end_turn") == llm::StopReason::EndTurn);
    CHECK(MapStopReason("max_tokens") == llm::StopReason::MaxTokens);
    CHECK(MapStopReason("stop_sequence") == llm::StopReason::StopSequence);
    CHECK(MapStopReason("tool_use") == llm::StopReason::ToolUse);
    CHECK(MapStopReason("refusal") == llm::StopReason::Refusal);
    CHECK(MapStopReason("pause_turn") == llm::StopReason::Unknown);
    CHECK(MapStopReason("something-new") == llm::StopReason::Unknown);
}

TEST_CASE("ParseUsage: direct fields, no nesting", "[anthropic_response]") {
    const auto usage = ParseUsage(nlohmann::json::parse(
        R"({"input_tokens":100,"output_tokens":20,"cache_read_input_tokens":80,)"
        R"("cache_creation_input_tokens":5})"));
    CHECK(usage.input_tokens == 100);
    CHECK(usage.output_tokens == 20);
    CHECK(usage.cache_read_input_tokens == 80);
    CHECK(usage.cache_creation_input_tokens == 5);
}

TEST_CASE("ParseUsage: absent fields default to zero, negative/float values are ignored",
         "[anthropic_response]") {
    const auto usage = ParseUsage(nlohmann::json::parse(R"({"input_tokens":-5})"));
    CHECK(usage.input_tokens == 0);
    CHECK(usage.output_tokens == 0);
}

TEST_CASE("ParseNonStreamingResponse: text and tool_use content", "[anthropic_response]") {
    const auto body = nlohmann::json::parse(R"({
        "id": "msg_1", "model": "claude-opus-5",
        "content": [
            {"type": "text", "text": "here you go"},
            {"type": "tool_use", "id": "toolu_1", "name": "read", "input": {"path": "a.txt"}}
        ],
        "stop_reason": "tool_use",
        "usage": {"input_tokens": 50, "output_tokens": 10}
    })");

    const auto response = ParseNonStreamingResponse(body);
    CHECK(response.id == "msg_1");
    CHECK(response.model == "claude-opus-5");
    CHECK(response.stop_reason == llm::StopReason::ToolUse);
    CHECK(response.usage.input_tokens == 50);
    REQUIRE(response.content.size() == 2);
    CHECK(std::get<llm::TextBlock>(response.content[0]).text == "here you go");
    const auto& tool_use = std::get<llm::ToolUseBlock>(response.content[1]);
    CHECK(tool_use.id == "toolu_1");
    CHECK(tool_use.name == "read");
    CHECK(tool_use.input["path"] == "a.txt");
}

TEST_CASE("ParseNonStreamingResponse: thinking blocks are dropped", "[anthropic_response]") {
    const auto body = nlohmann::json::parse(R"({
        "id": "msg_1", "model": "m",
        "content": [
            {"type": "thinking", "thinking": "reasoning...", "signature": "sig"},
            {"type": "text", "text": "answer"}
        ],
        "stop_reason": "end_turn"
    })");

    const auto response = ParseNonStreamingResponse(body);
    REQUIRE(response.content.size() == 1);
    CHECK(std::get<llm::TextBlock>(response.content[0]).text == "answer");
}

TEST_CASE("ParseNonStreamingResponse: missing content array is a parse error",
         "[anthropic_response]") {
    const auto body = nlohmann::json::parse(R"({"id": "msg_1", "model": "m"})");
    CHECK_THROWS_AS(ParseNonStreamingResponse(body), AnthropicParseError);
}

TEST_CASE("ParseNonStreamingResponse: malformed shape raises AnthropicParseError, not a raw "
         "json::exception",
         "[anthropic_response]") {
    const auto body = nlohmann::json::parse(R"({"content": [{"type": "tool_use"}]})");
    CHECK_THROWS_AS(ParseNonStreamingResponse(body), AnthropicParseError);
}
