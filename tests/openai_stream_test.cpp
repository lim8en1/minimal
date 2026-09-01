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

TEST_CASE("a stream cut before finish_reason still closes every block it opened",
          "[openai_stream][invariant]") {
    // ContentBlockStop was emitted ONLY inside the finish_reason branch, so an
    // idle timeout, a dropped connection or a cancelled sink left the client
    // holding a block it had been told about and would never be told ended --
    // the same failure the tool-lifecycle rule already names for the running-
    // tools panel, applied to content blocks.
    OpenAiStreamReassembler reassembler;
    std::vector<llm::StreamEvent> events;
    auto sink = CollectInto(events);

    REQUIRE(reassembler.FeedFrame(
        R"({"id":"c1","model":"m","choices":[{"delta":{"content":"partial"},"finish_reason":null}]})",
        sink));
    // ...and here the connection dies. No finish_reason, no [DONE].
    const std::size_t before = events.size();
    reassembler.CloseOpenBlocks(sink);

    REQUIRE(events.size() == before + 1);
    CHECK(Has<llm::ContentBlockStopEvent>(events, before));
    CHECK(As<llm::ContentBlockStopEvent>(events, before).block_index == 0);

    // Idempotent: a normal stream must not get two stops for one block.
    reassembler.CloseOpenBlocks(sink);
    CHECK(events.size() == before + 1);
}

TEST_CASE("a tool call cut before its id and name arrive is dropped, not half-emitted",
          "[openai_stream][invariant]") {
    // Finalize() emitted a ToolUseBlock with empty id and name, which reached
    // ToolExecutor as `unknown tool: ` -- an error attributed to a call the
    // model believes it made. Dropping it leaves nothing orphaned: there is no
    // tool_use in history to answer, and the loop already handles a ToolUse stop
    // reason with no blocks.
    OpenAiStreamReassembler reassembler;
    std::vector<llm::StreamEvent> events;
    auto sink = CollectInto(events);

    // Arguments arrive before the id/name -- a real ordering some proxies use --
    // and then the stream stops.
    REQUIRE(reassembler.FeedFrame(
        R"({"id":"c1","model":"m","choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"{\"a\":1}"}}]},"finish_reason":null}]})",
        sink));
    reassembler.CloseOpenBlocks(sink);

    const auto response = reassembler.Finalize();
    CHECK(response.content.empty());
}

TEST_CASE("cancellation tolerance: a tool call cut mid-arguments is dropped, not a parse error",
          "[openai_stream][invariant]") {
    // A user cancel landing while tool-call arguments were mid-JSON used to
    // surface as OpenAiParseError -> ParseError -- a non-retryable model error
    // and the wrong exit code, defeating the cancel-reporting fix for exactly
    // this subpath. In tolerance mode (the provider's cancel branch) the
    // truncated call is dropped the way a call whose id never arrived is:
    // there is no tool_use in history to answer, so nothing is orphaned, and
    // complete calls in the same stream survive.
    OpenAiStreamReassembler reassembler;
    std::vector<llm::StreamEvent> events;
    auto sink = CollectInto(events);

    // A COMPLETE call first -- it must survive the tolerant finalize.
    REQUIRE(reassembler.FeedFrame(
        R"({"id":"c1","model":"m","choices":[{"delta":{"tool_calls":[{"index":0,"id":"call1","function":{"name":"bash","arguments":"{\"cmd\":\"ls\"}"}}]},"finish_reason":null}]})",
        sink));
    // A second call whose arguments were cut mid-JSON (the arguments STRING is
    // invalid; the chunk carrying it stays parseable), then the stream stops.
    REQUIRE(reassembler.FeedFrame(
        R"({"id":"c1","model":"m","choices":[{"delta":{"tool_calls":[{"index":1,"id":"call2","function":{"name":"write","arguments":"{\"path\":\"a.cpp\""}}]},"finish_reason":null}]})",
        sink));
    reassembler.CloseOpenBlocks(sink);

    llm::ChatResponse response;
    REQUIRE_NOTHROW(response = reassembler.Finalize(/*tolerate_malformed_tool_calls=*/true));
    REQUIRE(response.content.size() == 1);
    const auto& kept = std::get<llm::ToolUseBlock>(response.content.front());
    CHECK(kept.id == "call1");
    CHECK(kept.input == nlohmann::json{{"cmd", "ls"}});

    // Strict mode -- a non-cancel exit -- still refuses the malformed call.
    CHECK_THROWS_AS(reassembler.Finalize(), OpenAiParseError);
}

TEST_CASE("completed(): finish_reason and [DONE] mark completion; a bare cut does not",
          "[openai_stream][invariant]") {
    // The provider turns a false here into a classified transport failure
    // instead of accepting StopReason::Unknown as a complete turn -- a proxy
    // truncating mid-generation with a tidy FIN used to produce a partial
    // assistant message persisted as ordinary history.
    OpenAiStreamReassembler reassembler;
    std::vector<llm::StreamEvent> events;
    auto sink = CollectInto(events);
    CHECK_FALSE(reassembler.completed());
    REQUIRE(reassembler.FeedFrame(
        R"({"id":"c1","model":"m","choices":[{"delta":{"content":"partial"},"finish_reason":null}]})",
        sink));
    CHECK_FALSE(reassembler.completed());
    REQUIRE(reassembler.FeedFrame(
        R"({"id":"c1","model":"m","choices":[{"delta":{},"finish_reason":"stop"}]})", sink));
    CHECK(reassembler.completed());

    OpenAiStreamReassembler done_terminated;
    std::vector<llm::StreamEvent> done_events;
    auto sink2 = CollectInto(done_events);
    REQUIRE(done_terminated.FeedFrame(
        R"({"id":"c2","model":"m","choices":[{"delta":{"content":"x"},"finish_reason":null}]})",
        sink2));
    REQUIRE(done_terminated.FeedFrame("[DONE]", sink2));
    CHECK(done_terminated.completed());
}

TEST_CASE("[DONE] with trailing whitespace completes instead of dying as a parse error",
          "[openai_stream]") {
    OpenAiStreamReassembler reassembler;
    std::vector<llm::StreamEvent> events;
    auto sink = CollectInto(events);
    REQUIRE(reassembler.FeedFrame(
        R"({"id":"c1","model":"m","choices":[{"delta":{"content":"x"},"finish_reason":null}]})",
        sink));
    REQUIRE_NOTHROW(reassembler.FeedFrame("[DONE] ", sink));
    CHECK(reassembler.completed());
}

TEST_CASE("text arriving after tool calls opens a new text block, never reorders",
          "[openai_stream][invariant]") {
    // The single text_block_index_ appended every content delta to the FIRST
    // text block: on backends that narrate between tool calls, later text was
    // re-emitted as a delta for a block the client had already seen closed,
    // and Finalize placed all text before all tool calls regardless of wire
    // order. Content order is now preserved, block for block.
    OpenAiStreamReassembler reassembler;
    std::vector<llm::StreamEvent> events;
    auto sink = CollectInto(events);

    REQUIRE(reassembler.FeedFrame(
        R"({"id":"c1","model":"m","choices":[{"delta":{"content":"step one: "}}]})", sink));
    REQUIRE(reassembler.FeedFrame(
        R"({"id":"c1","model":"m","choices":[{"delta":{"tool_calls":[{"index":0,"id":"t1","function":{"name":"bash","arguments":"{}"}}]}}]})",
        sink));
    REQUIRE(reassembler.FeedFrame(
        R"({"id":"c1","model":"m","choices":[{"delta":{"content":"step two: "}}]})", sink));
    REQUIRE(reassembler.FeedFrame(
        R"({"id":"c1","model":"m","choices":[{"delta":{},"finish_reason":"tool_calls"}]})", sink));

    const llm::ChatResponse response = reassembler.Finalize();
    REQUIRE(response.content.size() == 3);
    CHECK(std::get<llm::TextBlock>(response.content[0]).text == "step one: ");
    CHECK(std::get<llm::ToolUseBlock>(response.content[1]).id == "t1");
    CHECK(std::get<llm::TextBlock>(response.content[2]).text == "step two: ");
}

TEST_CASE("accumulated stream content is capped, however slowly it drips",
          "[openai_stream][invariant]") {
    // The 1 MiB SSE caps bound individual LINES; nothing bounded the TOTAL.
    // A server dripping one small frame just under every idle timeout grew the
    // reassembly strings at its drip rate forever while the worker hung. 64
    // MiB is ~16x a legitimate 1M-token response.
    OpenAiStreamReassembler reassembler;
    std::vector<llm::StreamEvent> drip_events;
    auto sink = CollectInto(drip_events);
    const std::string payload(1ull << 20, 'a');  // 1 MiB per frame
    std::string frame_data = R"({"id":"c1","model":"m","choices":[{"delta":{"content":")";
    // Escape-free padding: 'a' needs no JSON escaping.
    std::string frame_tail = R"("}}]})";

    bool threw = false;
    try {
        for (int i = 0; i < 80; ++i) {  // 80 MiB attempted: double the cap
            reassembler.FeedFrame(frame_data + payload + frame_tail, sink);
        }
    } catch (const OpenAiParseError& error) {
        threw = true;
        CHECK(std::string(error.what()).find("64 MiB") != std::string::npos);
    }
    CHECK(threw);
}
