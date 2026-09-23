#include <catch2/catch_test_macros.hpp>

#include "anthropic_response.hpp"
#include "anthropic_stream_reassembler.hpp"

using namespace minimal;
using namespace minimal::provider_anthropic;

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

TEST_CASE("AnthropicStreamReassembler: text-only stream", "[anthropic_stream]") {
    AnthropicStreamReassembler reassembler;
    std::vector<llm::StreamEvent> events;
    auto sink = CollectInto(events);

    REQUIRE(reassembler.FeedFrame(
        R"({"type":"message_start","message":{"id":"msg_1","model":"claude-opus-5",)"
        R"("usage":{"input_tokens":10,"output_tokens":1}}})",
        sink));
    REQUIRE(reassembler.FeedFrame(
        R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})",
        sink));
    REQUIRE(reassembler.FeedFrame(
        R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"Hel"}})",
        sink));
    REQUIRE(reassembler.FeedFrame(
        R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"lo"}})",
        sink));
    REQUIRE(reassembler.FeedFrame(R"({"type":"content_block_stop","index":0})", sink));
    REQUIRE(reassembler.FeedFrame(
        R"({"type":"message_delta","delta":{"stop_reason":"end_turn"},)"
        R"("usage":{"output_tokens":2}})",
        sink));
    REQUIRE(reassembler.FeedFrame(R"({"type":"message_stop"})", sink));

    // message_start emits Start + Usage; content_block_start (text) emits
    // nothing; two deltas; one stop; message_delta emits Delta + Usage;
    // message_stop emits Stop.
    REQUIRE(events.size() == 8);
    CHECK(Has<llm::MessageStartEvent>(events, 0));
    CHECK(Has<llm::UsageEvent>(events, 1));
    CHECK(As<llm::TextDeltaEvent>(events, 2).text == "Hel");
    CHECK(As<llm::TextDeltaEvent>(events, 3).text == "lo");
    CHECK(Has<llm::ContentBlockStopEvent>(events, 4));
    CHECK(As<llm::MessageDeltaEvent>(events, 5).stop_reason == llm::StopReason::EndTurn);
    CHECK(As<llm::UsageEvent>(events, 6).usage.output_tokens == 2);
    CHECK(Has<llm::MessageStopEvent>(events, 7));

    REQUIRE(reassembler.completed());
    const llm::ChatResponse response = reassembler.Finalize();
    CHECK(response.id == "msg_1");
    CHECK(response.model == "claude-opus-5");
    CHECK(response.stop_reason == llm::StopReason::EndTurn);
    CHECK(response.usage.input_tokens == 10);
    CHECK(response.usage.output_tokens == 2);
    REQUIRE(response.content.size() == 1);
    CHECK(std::get<llm::TextBlock>(response.content[0]).text == "Hello");
}

TEST_CASE("AnthropicStreamReassembler: tool_use arrives with id/name upfront, no remapping "
         "needed",
         "[anthropic_stream]") {
    AnthropicStreamReassembler reassembler;
    std::vector<llm::StreamEvent> events;
    auto sink = CollectInto(events);

    REQUIRE(reassembler.FeedFrame(
        R"({"type":"message_start","message":{"id":"msg_2","model":"m","usage":{}}})", sink));
    REQUIRE(reassembler.FeedFrame(
        R"({"type":"content_block_start","index":0,)"
        R"("content_block":{"type":"tool_use","id":"toolu_1","name":"read","input":{}}})",
        sink));
    // Anthropic's ToolUseStartEvent fires from content_block_start ITSELF --
    // unlike OpenAI, which needs id and name to both arrive across separate
    // deltas before it can fire one. (message_start itself emits two events:
    // MessageStart and an initial Usage.)
    REQUIRE(events.size() == 3);
    const auto& start = As<llm::ToolUseStartEvent>(events, 2);
    CHECK(start.id == "toolu_1");
    CHECK(start.name == "read");

    REQUIRE(reassembler.FeedFrame(
        R"({"type":"content_block_delta","index":0,)"
        R"("delta":{"type":"input_json_delta","partial_json":"{\"path\":"}})",
        sink));
    REQUIRE(reassembler.FeedFrame(
        R"({"type":"content_block_delta","index":0,)"
        R"("delta":{"type":"input_json_delta","partial_json":"\"a.txt\"}"}})",
        sink));
    REQUIRE(reassembler.FeedFrame(R"({"type":"content_block_stop","index":0})", sink));
    REQUIRE(reassembler.FeedFrame(
        R"({"type":"message_delta","delta":{"stop_reason":"tool_use"}})", sink));
    REQUIRE(reassembler.FeedFrame(R"({"type":"message_stop"})", sink));

    const llm::ChatResponse response = reassembler.Finalize();
    REQUIRE(response.content.size() == 1);
    const auto& block = std::get<llm::ToolUseBlock>(response.content[0]);
    CHECK(block.id == "toolu_1");
    CHECK(block.name == "read");
    CHECK(block.input["path"] == "a.txt");
}

TEST_CASE("AnthropicStreamReassembler: thinking deltas stream live and survive Finalize",
          "[anthropic_stream]") {
    AnthropicStreamReassembler reassembler;
    std::vector<llm::StreamEvent> events;
    auto sink = CollectInto(events);

    REQUIRE(reassembler.FeedFrame(
        R"({"type":"message_start","message":{"id":"msg_3","model":"m","usage":{}}})", sink));
    REQUIRE(reassembler.FeedFrame(
        R"({"type":"content_block_start","index":0,"content_block":{"type":"thinking","thinking":""}})",
        sink));
    REQUIRE(reassembler.FeedFrame(
        R"({"type":"content_block_delta","index":0,)"
        R"("delta":{"type":"thinking_delta","thinking":"pondering"}})",
        sink));
    REQUIRE(reassembler.FeedFrame(R"({"type":"content_block_stop","index":0})", sink));
    REQUIRE(reassembler.FeedFrame(
        R"({"type":"content_block_start","index":1,"content_block":{"type":"text","text":""}})",
        sink));
    REQUIRE(reassembler.FeedFrame(
        R"({"type":"content_block_delta","index":1,"delta":{"type":"text_delta","text":"answer"}})",
        sink));
    REQUIRE(reassembler.FeedFrame(R"({"type":"content_block_stop","index":1})", sink));
    REQUIRE(reassembler.FeedFrame(
        R"({"type":"message_delta","delta":{"stop_reason":"end_turn"}})", sink));

    bool saw_thinking_delta = false;
    for (const auto& event : events) {
        if (std::holds_alternative<llm::ThinkingDeltaEvent>(event)) saw_thinking_delta = true;
    }
    CHECK(saw_thinking_delta);   // the client still sees it live, for UI display

    const llm::ChatResponse response = reassembler.Finalize();
    REQUIRE(response.content.size() == 2);
    // Unsigned thinking is captured faithfully (scrollback shows it); the
    // REQUEST builder is what drops it at replay time.
    CHECK(std::get<llm::ThinkingBlock>(response.content[0]).text == "pondering");
    CHECK_FALSE(std::get<llm::ThinkingBlock>(response.content[0]).signature.has_value());
    CHECK(std::get<llm::TextBlock>(response.content[1]).text == "answer");
}

TEST_CASE("AnthropicStreamReassembler: signature_delta fragments concatenate into the block",
          "[anthropic_stream]") {
    AnthropicStreamReassembler reassembler;
    std::vector<llm::StreamEvent> events;
    auto sink = CollectInto(events);

    REQUIRE(reassembler.FeedFrame(
        R"({"type":"message_start","message":{"id":"msg_6","model":"m","usage":{}}})", sink));
    REQUIRE(reassembler.FeedFrame(
        R"({"type":"content_block_start","index":0,"content_block":{"type":"thinking","thinking":""}})",
        sink));
    REQUIRE(reassembler.FeedFrame(
        R"({"type":"content_block_delta","index":0,)"
        R"("delta":{"type":"thinking_delta","thinking":"deep thought"}})",
        sink));
    REQUIRE(reassembler.FeedFrame(
        R"({"type":"content_block_delta","index":0,)"
        R"("delta":{"type":"signature_delta","signature":"sig-par"}})",
        sink));
    REQUIRE(reassembler.FeedFrame(
        R"({"type":"content_block_delta","index":0,)"
        R"("delta":{"type":"signature_delta","signature":"t-2"}})",
        sink));
    REQUIRE(reassembler.FeedFrame(R"({"type":"content_block_stop","index":0})", sink));
    REQUIRE(reassembler.FeedFrame(
        R"({"type":"message_delta","delta":{"stop_reason":"end_turn"}})", sink));

    // signature_delta emits no stream event of its own -- the client needs
    // only the thinking text; the signature is replay bookkeeping.
    bool saw_non_delta_from_signature = false;
    for (const auto& event : events) {
        if (!std::holds_alternative<llm::ThinkingDeltaEvent>(event) &&
            !std::holds_alternative<llm::MessageStartEvent>(event) &&
            !std::holds_alternative<llm::UsageEvent>(event) &&
            !std::holds_alternative<llm::ContentBlockStopEvent>(event) &&
            !std::holds_alternative<llm::MessageDeltaEvent>(event)) {
            saw_non_delta_from_signature = true;
        }
    }
    CHECK_FALSE(saw_non_delta_from_signature);

    const llm::ChatResponse response = reassembler.Finalize();
    REQUIRE(response.content.size() == 1);
    const auto& thinking = std::get<llm::ThinkingBlock>(response.content[0]);
    CHECK(thinking.text == "deep thought");
    REQUIRE(thinking.signature.has_value());
    CHECK(*thinking.signature == "sig-part-2");   // concatenated, not overwritten
}

TEST_CASE("AnthropicStreamReassembler: redacted_thinking captures its data blob",
          "[anthropic_stream]") {
    AnthropicStreamReassembler reassembler;
    std::vector<llm::StreamEvent> events;
    auto sink = CollectInto(events);

    REQUIRE(reassembler.FeedFrame(
        R"({"type":"message_start","message":{"id":"msg_7","model":"m","usage":{}}})", sink));
    REQUIRE(reassembler.FeedFrame(
        R"({"type":"content_block_start","index":0,)"
        R"("content_block":{"type":"redacted_thinking","data":"encrypted-blob"}})",
        sink));
    REQUIRE(reassembler.FeedFrame(R"({"type":"content_block_stop","index":0})", sink));
    REQUIRE(reassembler.FeedFrame(
        R"({"type":"message_delta","delta":{"stop_reason":"end_turn"}})", sink));

    const llm::ChatResponse response = reassembler.Finalize();
    REQUIRE(response.content.size() == 1);
    const auto& thinking = std::get<llm::ThinkingBlock>(response.content[0]);
    CHECK(thinking.text.empty());
    CHECK_FALSE(thinking.signature.has_value());
    REQUIRE(thinking.redacted_data.has_value());
    CHECK(*thinking.redacted_data == "encrypted-blob");
}

TEST_CASE("AnthropicStreamReassembler: CloseOpenBlocks closes an announced but unstopped block",
         "[anthropic_stream]") {
    AnthropicStreamReassembler reassembler;
    std::vector<llm::StreamEvent> events;
    auto sink = CollectInto(events);

    REQUIRE(reassembler.FeedFrame(
        R"({"type":"message_start","message":{"id":"msg_4","model":"m","usage":{}}})", sink));
    REQUIRE(reassembler.FeedFrame(
        R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})",
        sink));
    REQUIRE(reassembler.FeedFrame(
        R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"cut off"}})",
        sink));
    // Connection drops here -- no content_block_stop, no message_delta.
    CHECK_FALSE(reassembler.completed());

    reassembler.CloseOpenBlocks(sink);
    CHECK(Has<llm::ContentBlockStopEvent>(events, events.size() - 1));

    // Idempotent: calling it again must not double-emit.
    const std::size_t count_before = events.size();
    reassembler.CloseOpenBlocks(sink);
    CHECK(events.size() == count_before);
}

TEST_CASE("AnthropicStreamReassembler: an unknown event type is a forward-compatible no-op",
         "[anthropic_stream]") {
    AnthropicStreamReassembler reassembler;
    std::vector<llm::StreamEvent> events;
    auto sink = CollectInto(events);
    CHECK(reassembler.FeedFrame(R"({"type":"a_future_event_type","stuff":123})", sink));
    CHECK(reassembler.FeedFrame(R"({"type":"ping"})", sink));
    CHECK(events.empty());
}

TEST_CASE("AnthropicStreamReassembler: malformed JSON throws AnthropicParseError",
         "[anthropic_stream]") {
    AnthropicStreamReassembler reassembler;
    std::vector<llm::StreamEvent> events;
    auto sink = CollectInto(events);
    CHECK_THROWS_AS(reassembler.FeedFrame("not json", sink), AnthropicParseError);
}

TEST_CASE("AnthropicStreamReassembler: sink returning false stops feeding and is surfaced",
         "[anthropic_stream]") {
    AnthropicStreamReassembler reassembler;
    llm::StreamEventSink refusing_sink = [](const llm::StreamEvent&) { return false; };
    CHECK_FALSE(reassembler.FeedFrame(
        R"({"type":"message_start","message":{"id":"msg_5","model":"m","usage":{}}})",
        refusing_sink));
}

TEST_CASE("AnthropicStreamReassembler: an implausible content_block_stop index throws, "
         "like every other block-indexed event",
         "[anthropic_stream]") {
    // content_block_start/content_block_delta route their index through
    // CheckedBlockIndex; content_block_stop read `index` directly until this
    // fix, so a negative wire index (wrapping to a huge size_t) or an index
    // past kMaxBlocks silently passed through as a plausible-looking
    // ContentBlockStopEvent instead of raising the same ParseError.
    AnthropicStreamReassembler reassembler;
    std::vector<llm::StreamEvent> events;
    auto sink = CollectInto(events);
    CHECK_THROWS_AS(
        reassembler.FeedFrame(R"({"type":"content_block_stop","index":-1})", sink),
        AnthropicParseError);

    AnthropicStreamReassembler reassembler2;
    CHECK_THROWS_AS(
        reassembler2.FeedFrame(R"({"type":"content_block_stop","index":999999})", sink),
        AnthropicParseError);
}

TEST_CASE("AnthropicStreamReassembler: present-but-null fields degrade, never throw",
          "[anthropic_stream][invariant]") {
    // value() on a key that is PRESENT but null throws type_error -- only
    // absence takes the default. The OpenAI reassembler documents the class:
    // a proxy emitting `"text": null` in a keepalive frame killed the turn
    // as a non-retryable ParseError. Every optional field here is guarded
    // the same way provider-openai sweeps its streaming path.
    AnthropicStreamReassembler reassembler;
    std::vector<llm::StreamEvent> events;
    auto sink = CollectInto(events);

    // Null id/model and a null usage on message_start.
    REQUIRE(reassembler.FeedFrame(
        R"({"type":"message_start","message":{"id":null,"model":null,"usage":null}})", sink));
    // Null text / partial_json / thinking / signature in each delta flavour.
    REQUIRE(reassembler.FeedFrame(
        R"({"type":"content_block_start","index":null,"content_block":{"type":null,"text":null}})",
        sink));
    REQUIRE(reassembler.FeedFrame(
        R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":null}})",
        sink));
    REQUIRE(reassembler.FeedFrame(
        R"({"type":"content_block_delta","index":0,"delta":{"type":"input_json_delta","partial_json":null}})",
        sink));
    REQUIRE(reassembler.FeedFrame(
        R"({"type":"content_block_delta","index":0,"delta":{"type":"thinking_delta","thinking":null}})",
        sink));
    REQUIRE(reassembler.FeedFrame(
        R"({"type":"content_block_delta","index":0,"delta":{"type":"signature_delta","signature":null}})",
        sink));
    REQUIRE(reassembler.FeedFrame(R"({"type":"content_block_stop","index":null})", sink));
    REQUIRE(reassembler.FeedFrame(
        R"({"type":"message_delta","delta":{"stop_reason":null},"usage":null})", sink));
    REQUIRE(reassembler.FeedFrame(R"({"type":"message_stop"})", sink));

    // The stream completed as a valid empty message, not a ParseError.
    const auto response = reassembler.Finalize(/*tolerate_malformed_tool_calls=*/false);
    CHECK(response.content.empty());
    CHECK(response.stop_reason == llm::StopReason::Unknown);
}
