#include "anthropic_stream_reassembler.hpp"

#include <nlohmann/json.hpp>

#include "anthropic_response.hpp"
#include "minimal/llm/json_depth.hpp"

namespace minimal::provider_anthropic {
namespace {

// Total content one stream may accumulate -- text, thinking, and tool-input
// fragments. Same reasoning and same cap as provider-openai's: the SSE
// parser bounds individual LINES, nothing bounds the TOTAL, and a server
// drip-feeding just under the idle timeout would otherwise grow this
// forever while the session worker hangs.
constexpr std::size_t kMaxAccumulatedBytes = 64ull << 20;

void Accumulate(std::size_t& total, std::size_t bytes) {
    total += bytes;
    if (total > kMaxAccumulatedBytes) {
        throw AnthropicParseError("stream content exceeded 64 MiB; refusing to accumulate more");
    }
}

// The content_block index comes straight off the wire, so it is
// attacker-influenced -- and blocks_.resize(index + 1) trusted it: an index
// of SIZE_MAX overflowed the addition, and even a "modest" 5,000,000
// allocated ~700 MB of Blocks in one step, bypassing the 64 MiB budget
// Accumulate() enforces on delta CONTENT (the Block vector itself is not
// content). No real message approaches this many blocks, so anything at or
// beyond the ceiling is a malformed stream, reported on the same
// AnthropicParseError channel as bad frame JSON.
constexpr std::size_t kMaxBlocks = 4096;

std::size_t CheckedBlockIndex(std::size_t index) {
    if (index + 1 < index || index >= kMaxBlocks) {
        throw AnthropicParseError("stream event named an implausible content_block index (" +
                                  std::to_string(index) + ")");
    }
    return index;
}

}  // namespace

bool AnthropicStreamReassembler::FeedFrame(std::string_view data, const llm::StreamEventSink& sink) {
    nlohmann::json event;
    try {
        if (!llm::JsonDepthWithinLimit(data)) {
            throw AnthropicParseError("stream event exceeds the JSON nesting limit");
        }
        event = nlohmann::json::parse(data);
    } catch (const nlohmann::json::exception& error) {
        throw AnthropicParseError(std::string("malformed stream event: ") + error.what());
    }

    const std::string type = event.value("type", "");

    if (type == "message_start") {
        const auto& message = event.at("message");
        id_ = message.value("id", "");
        model_ = message.value("model", "");
        message_started_ = true;
        if (message.contains("usage") && !message.at("usage").is_null()) {
            usage_ = ParseUsage(message.at("usage"));
        }
        if (sink && !sink(llm::MessageStartEvent{id_, model_})) return false;
        if (sink && !sink(llm::UsageEvent{usage_})) return false;
        return true;
    }

    if (type == "content_block_start") {
        const std::size_t index = CheckedBlockIndex(event.value("index", static_cast<std::size_t>(0)));
        const auto& content_block = event.at("content_block");
        const std::string block_type = content_block.value("type", "");
        if (index >= blocks_.size()) blocks_.resize(index + 1);

        if (block_type == "tool_use") {
            blocks_[index].kind = Block::Kind::ToolUse;
            blocks_[index].id = content_block.value("id", "");
            blocks_[index].name = content_block.value("name", "");
            if (sink &&
                !sink(llm::ToolUseStartEvent{static_cast<int>(index), blocks_[index].id,
                                             blocks_[index].name})) {
                return false;
            }
        } else if (block_type == "thinking" || block_type == "redacted_thinking") {
            blocks_[index].kind = Block::Kind::Thinking;
            // No dedicated "block started" event for text/thinking in
            // stream_event.hpp -- the first delta implies it, same as
            // provider-openai's text blocks.
        } else {
            blocks_[index].kind = Block::Kind::Text;
        }
        return true;
    }

    if (type == "content_block_delta") {
        const std::size_t index = CheckedBlockIndex(event.value("index", static_cast<std::size_t>(0)));
        if (index >= blocks_.size()) blocks_.resize(index + 1);
        const auto& delta = event.at("delta");
        const std::string delta_type = delta.value("type", "");

        if (delta_type == "text_delta") {
            std::string text = delta.value("text", "");
            Accumulate(accumulated_bytes_, text.size());
            blocks_[index].text += text;
            if (sink && !sink(llm::TextDeltaEvent{static_cast<int>(index), text})) return false;
        } else if (delta_type == "input_json_delta") {
            std::string partial = delta.value("partial_json", "");
            Accumulate(accumulated_bytes_, partial.size());
            blocks_[index].input_json += partial;
            if (sink && !sink(llm::ToolUseInputDeltaEvent{static_cast<int>(index), partial})) {
                return false;
            }
        } else if (delta_type == "thinking_delta") {
            std::string text = delta.value("thinking", "");
            Accumulate(accumulated_bytes_, text.size());
            blocks_[index].text += text;
            if (sink && !sink(llm::ThinkingDeltaEvent{static_cast<int>(index), text})) return false;
        }
        // "signature_delta": the thinking block's signature -- irrelevant
        // here since thinking is dropped at Finalize (never replayed).
        return true;
    }

    if (type == "content_block_stop") {
        const std::size_t index = event.value("index", static_cast<std::size_t>(0));
        if (index < blocks_.size()) blocks_[index].stopped = true;
        if (sink && !sink(llm::ContentBlockStopEvent{static_cast<int>(index)})) return false;
        return true;
    }

    if (type == "message_delta") {
        if (event.contains("delta")) {
            const auto& delta = event.at("delta");
            if (delta.contains("stop_reason") && !delta.at("stop_reason").is_null()) {
                stop_reason_ = MapStopReason(delta.at("stop_reason").get<std::string>());
                // Some servers never send content_block_stop and end the
                // message directly. The sink must not keep dangling open
                // blocks past the end-of-message signal, so emit the missing
                // stops BEFORE MessageDeltaEvent (blocks close, then the
                // message ends -- the same order a well-formed stream uses).
                EmitStopsForOpenBlocks(sink);
                finished_ = true;
                completed_ = true;
                if (sink && !sink(llm::MessageDeltaEvent{stop_reason_})) return false;
            }
        }
        if (event.contains("usage") && !event.at("usage").is_null()) {
            // Anthropic reports output_tokens (and, on some backends, revised
            // cache figures) here; SafeTokenCount-guarded fields that are
            // absent leave the message_start value in place rather than
            // zeroing it -- ParseUsage always returns a full Usage, so merge
            // field-by-field instead of overwriting wholesale.
            const llm::Usage delta_usage = ParseUsage(event.at("usage"));
            if (delta_usage.output_tokens > 0) usage_.output_tokens = delta_usage.output_tokens;
            if (delta_usage.input_tokens > 0) usage_.input_tokens = delta_usage.input_tokens;
            if (delta_usage.cache_read_input_tokens > 0) {
                usage_.cache_read_input_tokens = delta_usage.cache_read_input_tokens;
            }
            if (delta_usage.cache_creation_input_tokens > 0) {
                usage_.cache_creation_input_tokens = delta_usage.cache_creation_input_tokens;
            }
            if (sink && !sink(llm::UsageEvent{usage_})) return false;
        }
        return true;
    }

    if (type == "message_stop") {
        completed_ = true;
        if (sink && !sink(llm::MessageStopEvent{})) return false;
        return true;
    }

    // "ping" (keep-alive) and any future/unrecognized event type: no-op,
    // forward-compatible rather than a parse failure.
    return true;
}

void AnthropicStreamReassembler::EmitStopsForOpenBlocks(const llm::StreamEventSink& sink) {
    for (std::size_t i = 0; i < blocks_.size(); ++i) {
        if (!blocks_[i].stopped) {
            blocks_[i].stopped = true;
            if (sink && !sink(llm::ContentBlockStopEvent{static_cast<int>(i)})) return;
        }
    }
}

void AnthropicStreamReassembler::CloseOpenBlocks(const llm::StreamEventSink& sink) {
    // Idempotent against the message_delta path via finished_, same as
    // provider-openai's CloseOpenBlocks.
    if (finished_) return;
    finished_ = true;
    EmitStopsForOpenBlocks(sink);
}

llm::ChatResponse AnthropicStreamReassembler::Finalize(bool tolerate_malformed_tool_calls) const {
    llm::ChatResponse response;
    response.id = id_;
    response.model = model_;
    response.stop_reason = stop_reason_;
    response.usage = usage_;

    for (const auto& block : blocks_) {
        if (block.kind == Block::Kind::Text) {
            if (!block.text.empty()) response.content.push_back(llm::TextBlock{block.text});
        } else if (block.kind == Block::Kind::ToolUse) {
            if (block.id.empty() || block.name.empty()) continue;   // never fully arrived; drop, see below
            llm::ToolUseBlock tool_use;
            tool_use.id = block.id;
            tool_use.name = block.name;
            try {
                if (!block.input_json.empty() &&
                    !llm::JsonDepthWithinLimit(block.input_json)) {
                    throw AnthropicParseError("tool-use input exceeds the JSON nesting limit");
                }
                tool_use.input = block.input_json.empty() ? nlohmann::json::object()
                                                          : nlohmann::json::parse(block.input_json);
            } catch (const nlohmann::json::exception& error) {
                // Cancellation tolerance, same posture as provider-openai:
                // a stream the SINK cut mid-arguments reports Cancelled, not
                // a parse failure -- the truncated call carries no decision
                // the transcript needed, and dropping it orphans nothing
                // (no tool_use lands in history for it).
                if (tolerate_malformed_tool_calls) continue;
                throw AnthropicParseError(std::string("malformed tool-use input: ") + error.what());
            }
            response.content.push_back(std::move(tool_use));
        }
        // Kind::Thinking: dropped -- see anthropic_request.hpp's header
        // comment on why replay is unsafe without a populated signature.
    }
    return response;
}

}  // namespace minimal::provider_anthropic
