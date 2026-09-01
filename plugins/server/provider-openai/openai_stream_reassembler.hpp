#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "minimal/llm/chat.hpp"
#include "minimal/llm/stream_event.hpp"

namespace minimal::provider_openai {

// Per-call state machine -- exactly one instance per StreamComplete() call,
// never reused across calls (LlmProvider's "no mutable state across calls"
// contract). Fed the `data` payload of every SseFrame SseParser produces for
// the call, including the literal "[DONE]" frame. Absorbs OpenAI's flatter,
// tool_calls[].index-keyed chunk shape and emits StreamEvents at Anthropic's
// granularity, while separately accumulating enough state to hand back a full
// ChatResponse from Finalize().
class OpenAiStreamReassembler {
public:
    // Returns false the instant `sink` returns false -- the caller must stop
    // feeding further frames immediately and treat the call as cancelled.
    // Throws OpenAiParseError on malformed chunk JSON.
    bool FeedFrame(std::string_view data, const llm::StreamEventSink& sink);

    // Callable at any point (after "[DONE]", after frames run out, or after
    // FeedFrame returned false) to get the response assembled so far. Throws
    // OpenAiParseError if an accumulated tool-call `arguments` string isn't
    // valid JSON -- unless tolerate_malformed_tool_calls, the CANCELLATION
    // mode: a user cancel landing mid-arguments is reported as Cancelled, not
    // as a parse error, and the truncated call is dropped the way a call whose
    // id never arrived is (there is no tool_use in history to answer, so
    // nothing is orphaned; complete calls are kept).
    // Emits ContentBlockStop for every block still open, for a stream that
    // ended without a finish_reason (idle timeout, dropped connection, cancelled
    // sink). Idempotent with the finish_reason path.
    void CloseOpenBlocks(const llm::StreamEventSink& sink);

    llm::ChatResponse Finalize(bool tolerate_malformed_tool_calls = false) const;

    // The stream PROPERLY completed: a finish_reason was processed, or [DONE]
    // arrived. A clean connection close without either is a truncated
    // generation -- the provider turns a false here into a classified
    // transport failure instead of accepting StopReason::Unknown as a
    // complete turn.
    bool completed() const { return completed_; }

private:
    struct Block {
        enum class Kind { Text, ToolCall } kind;
        std::string text;                       // Kind::Text
        std::string id, name, arguments_json;   // Kind::ToolCall
        bool started = false;                   // Kind::ToolCall: ToolUseStartEvent already emitted
    };

    bool message_started_ = false;
    bool finished_ = false;   // finish_reason already processed -- blocks closed, MessageDeltaEvent sent
    bool completed_ = false;  // finish_reason OR [DONE]: the stream owes nothing
    std::string id_, model_;
    std::vector<Block> blocks_;                    // insertion order == first-seen order
    std::optional<std::size_t> text_block_index_;
    std::map<int, std::size_t> tool_call_indices_;  // OpenAI tool_calls[].index -> blocks_ index
    std::size_t accumulated_bytes_ = 0;  // text + arguments content, capped -- see kMaxAccumulatedBytes
    llm::StopReason stop_reason_ = llm::StopReason::Unknown;
    llm::Usage usage_;
};

}  // namespace minimal::provider_openai
