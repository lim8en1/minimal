#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "minimal/llm/chat.hpp"
#include "minimal/llm/stream_event.hpp"

namespace minimal::provider_anthropic {

// Per-call state machine -- exactly one instance per StreamComplete() call,
// never reused (LlmProvider's "no mutable state across calls" contract).
// Fed the `data` payload of every SseFrame the SSE parser produces.
//
// Anthropic's content_block_start/delta/stop events are ALREADY indexed
// exactly the way llm::StreamEvent's block_index expects, and content_block
// _start for a tool_use carries its id/name upfront -- unlike OpenAI's flat,
// tool_calls[].index-keyed chunks, which need an index-remapping table and
// drip id/name across several deltas before a ToolUseStartEvent can even
// fire. This is the "close to a transliteration" future stream_event.hpp
// predicted: no remapping table, no started-once-id-and-name-both-arrive
// bookkeeping -- content_block_start already IS the block's identity.
class AnthropicStreamReassembler {
public:
    // Returns false the instant `sink` returns false -- the caller must stop
    // feeding further frames immediately and treat the call as cancelled.
    // Throws AnthropicParseError on malformed frame JSON. `event: error`
    // frames are the PROVIDER's job to classify and throw before calling
    // this (same split as provider-openai's on_chunk / ClassifyErrorEventData).
    bool FeedFrame(std::string_view data, const llm::StreamEventSink& sink);

    // Callable at any point to close every block that never got its
    // content_block_stop -- an idle timeout, a dropped connection, or a
    // cancelled sink all leave announced blocks permanently open otherwise.
    // Idempotent with the normal message_stop path.
    void CloseOpenBlocks(const llm::StreamEventSink& sink);

    llm::ChatResponse Finalize(bool tolerate_malformed_tool_calls = false) const;

    // A stop_reason was processed (message_delta) OR message_stop arrived.
    // A clean connection close without either is a truncated generation, the
    // same posture provider-openai takes on a stream with no finish_reason.
    bool completed() const { return completed_; }

private:
    // Emits ContentBlockStopEvent for every announced-but-not-stopped block
    // and marks them stopped. Shared by CloseOpenBlocks and the message_delta
    // path: a server that ends the message without stopping its blocks must
    // not leave the SINK with dangling open blocks either.
    void EmitStopsForOpenBlocks(const llm::StreamEventSink& sink);

    struct Block {
        enum class Kind { Text, ToolUse, Thinking } kind;
        std::string text;                      // Kind::Text / Kind::Thinking
        std::optional<std::string> signature;  // Kind::Thinking: signature_delta fragments, concatenated
        std::optional<std::string> redacted_data;  // Kind::Thinking: redacted_thinking's data blob
        std::string id, name, input_json;      // Kind::ToolUse
        bool stopped = false;                  // content_block_stop already seen
    };

    bool message_started_ = false;
    bool finished_ = false;    // a stop_reason was processed -- blocks closed, MessageDeltaEvent sent
    bool completed_ = false;
    std::string id_, model_;
    std::vector<Block> blocks_;   // indexed by Anthropic's OWN content_block index -- no remapping
    std::size_t accumulated_bytes_ = 0;   // text + thinking + tool input, capped
    llm::StopReason stop_reason_ = llm::StopReason::Unknown;
    llm::Usage usage_;
};

}  // namespace minimal::provider_anthropic
