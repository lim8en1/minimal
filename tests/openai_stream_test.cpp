#include <catch2/catch_test_macros.hpp>

#include "openai_response.hpp"
#include "openai_stream_reassembler.hpp"

using namespace minimal;
using namespace minimal::provider_openai;

namespace {

llm::StreamEventSink CollectInto(std::vector<llm::StreamEvent>& out) {
    return [&out](const llm::StreamEvent& event) {
        out.push_back(event);
        return true;
    };
}

template <typename T>
bool Has(const std::vector<llm::StreamEvent>& events, std::size_t index) {
    return std::holds_alternative<T>(events.at(index));
}

template <typename T>
const T& As(const std::vector<llm::StreamEvent>& events, std::size_t index) {
    return std::get<T>(events.at(index));
}

}  // namespace

TEST_CASE("OpenAiStreamReassembler: text-only stream", "[openai_stream]") {
    OpenAiStreamReassembler reassembler;
    std::vector<llm::StreamEvent> events;
    auto sink = CollectInto(events);

    REQUIRE(reassembler.FeedFrame(R"({"id":"c1","model":"m","choices":[{"delta":{"content":"Hel"},"finish_reason":null}]})", sink));
    REQUIRE(reassembler.FeedFrame(R"({"choices":[{"delta":{"content":"lo"},"finish_reason":null}]})", sink));
    REQUIRE(reassembler.FeedFrame(R"({"choices":[{"delta":{},"finish_reason":"stop"}]})", sink));
    REQUIRE(reassembler.FeedFrame("[DONE]", sink));

    REQUIRE(events.size() == 6);
    CHECK(Has<llm::MessageStartEvent>(events, 0));
    CHECK(As<llm::TextDeltaEvent>(events, 1).text == "Hel");
    CHECK(As<llm::TextDeltaEvent>(events, 2).text == "lo");
    CHECK(Has<llm::ContentBlockStopEvent>(events, 3));
    CHECK(As<llm::MessageDeltaEvent>(events, 4).stop_reason == llm::StopReason::EndTurn);
    CHECK(Has<llm::MessageStopEvent>(events, 5));

    llm::ChatResponse response = reassembler.Finalize();
    CHECK(response.id == "c1");
    CHECK(response.model == "m");
    CHECK(response.stop_reason == llm::StopReason::EndTurn);
    REQUIRE(response.content.size() == 1);
    CHECK(std::get<llm::TextBlock>(response.content[0]).text == "Hello");
}

TEST_CASE("OpenAiStreamReassembler: single tool-call stream", "[openai_stream]") {
    OpenAiStreamReassembler reassembler;
    std::vector<llm::StreamEvent> events;
    auto sink = CollectInto(events);

    REQUIRE(reassembler.FeedFrame(
        R"({"id":"c2","model":"m","choices":[{"delta":{"tool_calls":[)"
        R"({"index":0,"id":"call_1","type":"function","function":{"name":"read","arguments":""}}]},)"
        R"("finish_reason":null}]})",
        sink));
    REQUIRE(reassembler.FeedFrame(
        R"({"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"{\"path\":"}}]},)"
        R"("finish_reason":null}]})",
        sink));
    REQUIRE(reassembler.FeedFrame(
        R"({"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"\"a.txt\"}"}}]},)"
        R"("finish_reason":null}]})",
        sink));
    REQUIRE(reassembler.FeedFrame(R"({"choices":[{"delta":{},"finish_reason":"tool_calls"}]})", sink));
    REQUIRE(reassembler.FeedFrame("[DONE]", sink));

    REQUIRE(events.size() == 7);
    CHECK(Has<llm::MessageStartEvent>(events, 0));
    const auto& start = As<llm::ToolUseStartEvent>(events, 1);
    CHECK(start.id == "call_1");
    CHECK(start.name == "read");
    CHECK(As<llm::ToolUseInputDeltaEvent>(events, 2).partial_json == R"({"path":)");
    CHECK(As<llm::ToolUseInputDeltaEvent>(events, 3).partial_json == R"("a.txt"})");
    CHECK(Has<llm::ContentBlockStopEvent>(events, 4));
    CHECK(As<llm::MessageDeltaEvent>(events, 5).stop_reason == llm::StopReason::ToolUse);

    llm::ChatResponse response = reassembler.Finalize();
    REQUIRE(response.content.size() == 1);
    const auto& block = std::get<llm::ToolUseBlock>(response.content[0]);
    CHECK(block.id == "call_1");
    CHECK(block.name == "read");
    CHECK(block.input == nlohmann::json{{"path", "a.txt"}});
}

TEST_CASE("OpenAiStreamReassembler: interleaved tool-call indices preserve first-seen order",
          "[openai_stream]") {
    OpenAiStreamReassembler reassembler;
    std::vector<llm::StreamEvent> events;
    auto sink = CollectInto(events);

    reassembler.FeedFrame(
        R"({"id":"c3","model":"m","choices":[{"delta":{"tool_calls":[)"
        R"({"index":0,"id":"call_1","function":{"name":"foo","arguments":""}}]},"finish_reason":null}]})",
        sink);
    reassembler.FeedFrame(
        R"({"choices":[{"delta":{"tool_calls":[)"
        R"({"index":1,"id":"call_2","function":{"name":"bar","arguments":""}}]},"finish_reason":null}]})",
        sink);
    reassembler.FeedFrame(
        R"({"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"{\"x\":1}"}}]},)"
        R"("finish_reason":null}]})",
        sink);
    reassembler.FeedFrame(
        R"({"choices":[{"delta":{"tool_calls":[{"index":1,"function":{"arguments":"{\"y\":2}"}}]},)"
        R"("finish_reason":null}]})",
        sink);
    reassembler.FeedFrame(R"({"choices":[{"delta":{},"finish_reason":"tool_calls"}]})", sink);

    llm::ChatResponse response = reassembler.Finalize();
    REQUIRE(response.content.size() == 2);
    const auto& first = std::get<llm::ToolUseBlock>(response.content[0]);
    const auto& second = std::get<llm::ToolUseBlock>(response.content[1]);
    CHECK(first.id == "call_1");
    CHECK(first.input == nlohmann::json{{"x", 1}});
    CHECK(second.id == "call_2");
    CHECK(second.input == nlohmann::json{{"y", 2}});
}

TEST_CASE("OpenAiStreamReassembler: usage chunk emits UsageEvent and feeds Finalize", "[openai_stream]") {
    OpenAiStreamReassembler reassembler;
    std::vector<llm::StreamEvent> events;
    auto sink = CollectInto(events);

    reassembler.FeedFrame(R"({"id":"c4","model":"m","choices":[{"delta":{"content":"hi"},"finish_reason":"stop"}]})",
                           sink);
    REQUIRE(reassembler.FeedFrame(
        R"({"choices":[],"usage":{"prompt_tokens":10,"completion_tokens":5}})", sink));

    REQUIRE_FALSE(events.empty());
    const auto& usage_event = As<llm::UsageEvent>(events, events.size() - 1);
    CHECK(usage_event.usage.input_tokens == 10);
    CHECK(usage_event.usage.output_tokens == 5);

    llm::ChatResponse response = reassembler.Finalize();
    CHECK(response.usage.input_tokens == 10);
    CHECK(response.usage.output_tokens == 5);
}

TEST_CASE("OpenAiStreamReassembler: sink returning false stops feeding immediately", "[openai_stream]") {
    OpenAiStreamReassembler reassembler;
    std::vector<llm::StreamEvent> events;
    llm::StreamEventSink stop_after_start = [&events](const llm::StreamEvent& event) {
        events.push_back(event);
        return !std::holds_alternative<llm::MessageStartEvent>(event);
    };

    bool result = reassembler.FeedFrame(
        R"({"id":"c5","model":"m","choices":[{"delta":{"content":"hi"},"finish_reason":null}]})",
        stop_after_start);

    CHECK_FALSE(result);
    REQUIRE(events.size() == 1);
    CHECK(Has<llm::MessageStartEvent>(events, 0));
}

TEST_CASE("OpenAiStreamReassembler: malformed chunk JSON throws OpenAiParseError", "[openai_stream]") {
    OpenAiStreamReassembler reassembler;
    std::vector<llm::StreamEvent> events;
    auto sink = CollectInto(events);
    CHECK_THROWS_AS(reassembler.FeedFrame("not json", sink), OpenAiParseError);
}
