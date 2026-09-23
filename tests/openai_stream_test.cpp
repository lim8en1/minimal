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

TEST_CASE("explicit-null stream fields are tolerated like absent ones", "[openai_stream][invariant]") {
    // value() on present-but-null throws type_error, and the resulting
    // ParseError is NON-retryable: a proxy that emits `"index": null` (or
    // null id/name/arguments) in a keepalive-shaped chunk would fail the
    // whole turn instead of being ignored.
    OpenAiStreamReassembler reassembler;
    std::vector<llm::StreamEvent> events;
    auto sink = CollectInto(events);

    CHECK_NOTHROW(reassembler.FeedFrame(
        R"({"id":"c","model":"m","choices":[{"delta":{"tool_calls":[)"
        R"({"index":null,"id":null,"type":"function","function":{"name":null,"arguments":null}}]},)"
        R"("finish_reason":null}]})",
        sink));
    // Nothing startable arrived: no tool-use events, no crash.
    CHECK(events.size() == 1);  // MessageStart only
    CHECK(reassembler.Finalize().content.empty());
}

TEST_CASE("reasoning_content deltas stream live and survive Finalize as a ThinkingBlock",
          "[openai_stream]") {
    // DeepSeek/GLM-style OpenAI-compatible backends carry reasoning in
    // delta.reasoning_content. Without capture it was dropped outright --
    // no live delta, nothing persisted, nothing to show in the client.
    OpenAiStreamReassembler reassembler;
    std::vector<llm::StreamEvent> events;
    auto sink = CollectInto(events);

    REQUIRE(reassembler.FeedFrame(
        R"({"id":"c1","model":"m","choices":[{"delta":{"reasoning_content":"pondering"}}]})", sink));
    REQUIRE(reassembler.FeedFrame(
        R"({"choices":[{"delta":{"reasoning_content":" deeply"}}]})", sink));
    REQUIRE(reassembler.FeedFrame(
        R"({"choices":[{"delta":{"content":"The answer."}}]})", sink));
    REQUIRE(reassembler.FeedFrame(
        R"({"choices":[{"delta":{},"finish_reason":"stop"}]})", sink));

    REQUIRE(events.size() == 7);
    CHECK(Has<llm::MessageStartEvent>(events, 0));
    CHECK(As<llm::ThinkingDeltaEvent>(events, 1).text == "pondering");
    CHECK(As<llm::ThinkingDeltaEvent>(events, 2).text == " deeply");
    CHECK(As<llm::TextDeltaEvent>(events, 3).text == "The answer.");
    // Blocks close in index order at finish_reason: thinking (0), text (1).
    CHECK(As<llm::ContentBlockStopEvent>(events, 4).block_index == 0);
    CHECK(As<llm::ContentBlockStopEvent>(events, 5).block_index == 1);

    const llm::ChatResponse response = reassembler.Finalize();
    REQUIRE(response.content.size() == 2);
    const auto& thinking = std::get<llm::ThinkingBlock>(response.content[0]);
    CHECK(thinking.text == "pondering deeply");
    CHECK_FALSE(thinking.signature.has_value());
    CHECK(std::get<llm::TextBlock>(response.content[1]).text == "The answer.");
}

TEST_CASE("OpenRouter's reasoning spelling is captured the same way; present-but-null tolerated",
          "[openai_stream]") {
    OpenAiStreamReassembler reassembler;
    std::vector<llm::StreamEvent> events;
    auto sink = CollectInto(events);

    CHECK_NOTHROW(reassembler.FeedFrame(
        R"({"id":"c","model":"m","choices":[{"delta":{"reasoning_content":null,"reasoning":null}}]})",
        sink));
    REQUIRE(reassembler.FeedFrame(
        R"({"choices":[{"delta":{"reasoning":"hmm"}}]})", sink));
    REQUIRE(reassembler.FeedFrame(
        R"({"choices":[{"delta":{},"finish_reason":"stop"}]})", sink));

    const llm::ChatResponse response = reassembler.Finalize();
    REQUIRE(response.content.size() == 1);
    CHECK(std::get<llm::ThinkingBlock>(response.content[0]).text == "hmm");
}

TEST_CASE("reasoning -> text -> reasoning stays three blocks in arrival order",
          "[openai_stream][invariant]") {
    // The same open-a-new-block rule text-after-tool-calls follows: a second
    // reasoning stretch after content must not append onto the first one's
    // block, which would reorder it before the text in the persisted
    // transcript.
    OpenAiStreamReassembler reassembler;
    std::vector<llm::StreamEvent> events;
    auto sink = CollectInto(events);

    REQUIRE(reassembler.FeedFrame(
        R"({"id":"c1","model":"m","choices":[{"delta":{"reasoning_content":"first thought"}}]})", sink));
    REQUIRE(reassembler.FeedFrame(
        R"({"choices":[{"delta":{"content":"middle answer"}}]})", sink));
    REQUIRE(reassembler.FeedFrame(
        R"({"choices":[{"delta":{"reasoning_content":"second thought"}}]})", sink));
    REQUIRE(reassembler.FeedFrame(
        R"({"choices":[{"delta":{},"finish_reason":"stop"}]})", sink));

    const llm::ChatResponse response = reassembler.Finalize();
    REQUIRE(response.content.size() == 3);
    CHECK(std::get<llm::ThinkingBlock>(response.content[0]).text == "first thought");
    CHECK(std::get<llm::TextBlock>(response.content[1]).text == "middle answer");
    CHECK(std::get<llm::ThinkingBlock>(response.content[2]).text == "second thought");
}

TEST_CASE("an empty reasoning delta does not create a phantom ThinkingBlock", "[openai_stream]") {
    OpenAiStreamReassembler reassembler;
    std::vector<llm::StreamEvent> events;
    auto sink = CollectInto(events);

    REQUIRE(reassembler.FeedFrame(
        R"({"id":"c","model":"m","choices":[{"delta":{"reasoning_content":""}}]})", sink));
    CHECK(events.size() == 1);  // MessageStart only
    CHECK(reassembler.Finalize().content.empty());
}

TEST_CASE("a stream cut before finish_reason closes an open thinking block too",
          "[openai_stream][invariant]") {
    OpenAiStreamReassembler reassembler;
    std::vector<llm::StreamEvent> events;
    auto sink = CollectInto(events);

    REQUIRE(reassembler.FeedFrame(
        R"({"id":"c1","model":"m","choices":[{"delta":{"reasoning_content":"cut mid-thought"}}]})",
        sink));
    const std::size_t before = events.size();
    reassembler.CloseOpenBlocks(sink);

    REQUIRE(events.size() == before + 1);
    CHECK(Has<llm::ContentBlockStopEvent>(events, before));
    CHECK(As<llm::ContentBlockStopEvent>(events, before).block_index == 0);

    const llm::ChatResponse response = reassembler.Finalize();
    REQUIRE(response.content.size() == 1);
    CHECK(std::get<llm::ThinkingBlock>(response.content[0]).text == "cut mid-thought");
}

TEST_CASE("an empty content delta does not create a phantom TextBlock", "[openai_stream]") {
    OpenAiStreamReassembler reassembler;
    std::vector<llm::StreamEvent> events;
    auto sink = CollectInto(events);

    REQUIRE(reassembler.FeedFrame(
        R"({"id":"c","model":"m","choices":[{"delta":{"content":""},"finish_reason":null}]})",
        sink));
    // MessageStart only -- no TextDelta, no empty block.
    CHECK(events.size() == 1);
    CHECK(reassembler.Finalize().content.empty());
}

TEST_CASE("argument fragments that arrive before id+name are streamed at start",
          "[openai_stream][invariant]") {
    // They always accumulated into the final response, but the sink never saw
    // them: the streamed tool input missed its head until the next fragment.
    OpenAiStreamReassembler reassembler;
    std::vector<llm::StreamEvent> events;
    auto sink = CollectInto(events);

    // Fragment FIRST (no id/name yet), then the startable delta.
    REQUIRE(reassembler.FeedFrame(
        R"({"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"{\"a\":1}"}}]},"finish_reason":null}]})",
        sink));
    REQUIRE(reassembler.FeedFrame(
        R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_1","function":{"name":"read"}}]},"finish_reason":null}]})",
        sink));
    REQUIRE(reassembler.FeedFrame(R"({"choices":[{"delta":{},"finish_reason":"tool_calls"}]})", sink));

    // MessageStart, ToolUseStart, then the buffered fragment, then stop.
    REQUIRE(events.size() >= 4);
    CHECK(Has<llm::ToolUseStartEvent>(events, 1));
    CHECK(As<llm::ToolUseInputDeltaEvent>(events, 2).partial_json == R"({"a":1})");

    llm::ChatResponse response = reassembler.Finalize();
    REQUIRE(response.content.size() == 1);
    CHECK(std::get<llm::ToolUseBlock>(response.content[0]).input == nlohmann::json{{"a", 1}});
}

TEST_CASE("a deeply nested stream chunk is refused as a parse error, not a crash",
          "[openai_stream][invariant]") {
    // nlohmann's parse is recursive descent with no depth limit: a hostile
    // gateway's 1 MiB of '[' overflows the call stack. The guard refuses
    // first.
    OpenAiStreamReassembler reassembler;
    std::vector<llm::StreamEvent> events;
    auto sink = CollectInto(events);
    const std::string deep(600, '[');
    CHECK_THROWS_AS(reassembler.FeedFrame(deep, sink), OpenAiParseError);
    // A legitimate deep-ish payload passes.
    OpenAiStreamReassembler sane;
    CHECK_NOTHROW(sane.FeedFrame(R"({"a":[[{"b":[1,2,[3]]}]]})", sink));
}

TEST_CASE("a flood of distinct tool_call indices is refused, not accumulated without bound",
          "[openai_stream][invariant]") {
    // A new Block is pushed for every unseen tool_calls[].index with no
    // accompanying content -- unlike text/thinking deltas, a bare
    // {"index":N} frame advances none of the 64 MiB Accumulate() budget, so
    // that budget alone does not bound how many of these a stream can send.
    OpenAiStreamReassembler reassembler;
    std::vector<llm::StreamEvent> events;
    auto sink = CollectInto(events);
    REQUIRE_NOTHROW(reassembler.FeedFrame(
        R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_0","function":{"name":"f"}}]}}]})",
        sink));

    bool threw = false;
    try {
        for (int i = 1; i <= 5000; ++i) {
            const std::string frame =
                R"({"choices":[{"delta":{"tool_calls":[{"index":)" + std::to_string(i) + "}]}}]}";
            reassembler.FeedFrame(frame, sink);
        }
    } catch (const OpenAiParseError&) {
        threw = true;
    }
    CHECK(threw);
}

TEST_CASE("a flood of alternating 1-byte content/reasoning deltas is refused, not "
         "accumulated without bound",
         "[openai_stream][invariant]") {
    // Unlike the tool_calls flood above, each of these frames DOES carry a
    // byte of delta content -- but alternating content/reasoning_content
    // flips Block::Kind every frame, opening a NEW Block each time (the
    // existing-block-of-the-same-kind check fails), so tens of millions of
    // 1-byte deltas can grow blocks_ far past any real response's block
    // count while still trickling well under the 64 MiB Accumulate() budget.
    OpenAiStreamReassembler reassembler;
    std::vector<llm::StreamEvent> events;
    auto sink = CollectInto(events);

    bool threw = false;
    try {
        for (int i = 0; i < 5000; ++i) {
            const std::string frame = (i % 2 == 0)
                ? R"({"choices":[{"delta":{"content":"x"}}]})"
                : R"({"choices":[{"delta":{"reasoning_content":"x"}}]})";
            reassembler.FeedFrame(frame, sink);
        }
    } catch (const OpenAiParseError&) {
        threw = true;
    }
    CHECK(threw);
}
