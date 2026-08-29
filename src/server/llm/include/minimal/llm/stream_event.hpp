#pragma once

#include <functional>
#include <string>
#include <variant>

#include "minimal/llm/chat.hpp"
#include "minimal/llm/message.hpp"

namespace minimal::llm {

// Modeled at Anthropic's event granularity rather than OpenAI's flatter chunk
// shape. The OpenAI provider absorbs the extra bookkeeping (reassembling
// tool-call fragments keyed by index) so a future Anthropic provider is close to
// a transliteration -- the asymmetry is deliberate and points the right way,
// since the interface outlives any one provider.

struct MessageStartEvent {
    std::string id;
    std::string model;
};

struct TextDeltaEvent {
    int block_index = 0;
    std::string text;
};

struct ToolUseStartEvent {
    int block_index = 0;
    std::string id;
    std::string name;
};

struct ToolUseInputDeltaEvent {
    int block_index = 0;
    std::string partial_json;  // concatenate; parse at ContentBlockStop
};

struct ThinkingDeltaEvent {
    int block_index = 0;
    std::string text;
};

struct ContentBlockStopEvent {
    int block_index = 0;
};

struct MessageDeltaEvent {
    StopReason stop_reason = StopReason::Unknown;
};

struct UsageEvent {
    Usage usage;
};

struct MessageStopEvent {};

using StreamEvent = std::variant<MessageStartEvent, TextDeltaEvent, ToolUseStartEvent,
                                  ToolUseInputDeltaEvent, ThinkingDeltaEvent, ContentBlockStopEvent,
                                  MessageDeltaEvent, UsageEvent, MessageStopEvent>;

// Returns false to abort the in-flight request. Deliberately a return value
// rather than an exception: the abort propagates through libcurl's C write
// callback (which returns a short byte count, yielding CURLE_WRITE_ERROR), and
// throwing across C stack frames is undefined behavior.
using StreamEventSink = std::function<bool(const StreamEvent&)>;

}  // namespace minimal::llm
