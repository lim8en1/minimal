#include "openai_stream_reassembler.hpp"

#include <nlohmann/json.hpp>

#include "openai_response.hpp"
#include "minimal/llm/json_depth.hpp"
#include "minimal/common/units.hpp"
#include "minimal/llm/policy.hpp"

namespace minimal::provider_openai {
namespace {

// The total content one stream may accumulate -- text plus tool-call argument
// fragments. The SSE parser's 1 MiB caps bound individual LINES; nothing used
// to bound the TOTAL, so a server dripping one small frame just under every
// idle timeout grew these strings at its drip rate forever while the session
// worker hung (llm::kMaxStreamContentBytes).

void Accumulate(std::size_t& total, std::size_t bytes) {
    total += bytes;
    if (total > llm::kMaxStreamContentBytes) {
        throw OpenAiParseError("stream content exceeded " + std::to_string(llm::kMaxStreamContentBytes) +
                         " bytes; refusing to accumulate more");
    }
}

// tool_calls[].index comes straight off the wire, so it is
// attacker-influenced. A new Block is pushed for every unseen index with no
// bound of its own -- unlike delta CONTENT, which Accumulate() meters, the
// Block vector itself is not content, so a stream of bare
// {"tool_calls":[{"index":N}]} frames (no arguments fragment to advance the
// content budget) grew blocks_ and tool_call_indices_ by one entry per frame,
// unbounded. Same reasoning and same ceiling as provider-anthropic's
// CheckedBlockIndex (anthropic_stream_reassembler.cpp) -- no real response
// approaches this many tool calls, so anything at or beyond it is a
// malformed stream, reported on the same OpenAiParseError channel as bad
// frame JSON.

std::string_view TrimTrailing(std::string_view s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' ||
                          s.back() == '\n')) {
        s.remove_suffix(1);
    }
    return s;
}

}  // namespace

bool OpenAiStreamReassembler::FeedFrame(std::string_view data, const llm::StreamEventSink& sink) {
    // Trailing whitespace survives the parser's single-leading-space strip, and
    // "[DONE] " then died the whole stream as a parse error at the final frame.
    if (TrimTrailing(data) == "[DONE]") {
        completed_ = true;
        return !sink || sink(llm::MessageStopEvent{});
    }

    nlohmann::json chunk;
    try {
        if (!llm::JsonDepthWithinLimit(data)) {
            throw OpenAiParseError("stream chunk exceeds the JSON nesting limit");
        }
        chunk = nlohmann::json::parse(data);
    } catch (const nlohmann::json::exception& error) {
        throw OpenAiParseError(std::string("malformed stream chunk: ") + error.what());
    }

    if (!message_started_) {
        // value() on a key that is present-but-null throws type_error -- only
        // absence takes the default. A proxy sending `"id": null` killed the
        // turn as a ParseError; the streaming path guards (here and at
        // finish_reason/usage), the non-streaming one did not -- until the
        // guards were swept to both.
        const auto string_or = [](const nlohmann::json& json,
                                  const char* key) -> std::string {
            if (!json.contains(key) || json.at(key).is_null()) return {};
            return json.at(key).get<std::string>();
        };
        id_ = string_or(chunk, "id");
        model_ = string_or(chunk, "model");
        message_started_ = true;
        if (sink && !sink(llm::MessageStartEvent{id_, model_})) return false;
    }

    if (chunk.contains("choices") && !chunk.at("choices").empty()) {
        const auto& choice = chunk.at("choices").at(0);
        nlohmann::json delta = choice.value("delta", nlohmann::json::object());

        // Reasoning deltas, two wire spellings: "reasoning_content"
        // (DeepSeek/GLM and most OpenAI-compatible proxies) and "reasoning"
        // (OpenRouter's normalized field). Both are plain strings when
        // present; present-but-null is tolerated like every other delta
        // field. Captured as a Thinking block so the transcript and TUI get
        // the same treatment provider-anthropic gives thinking -- the
        // request builder drops ThinkingBlocks on replay (no signature
        // exists to replay), which is correct for chat-completions.
        const auto reasoning_fragment = [&]() -> std::string {
            for (const char* key : {"reasoning_content", "reasoning"}) {
                if (delta.contains(key) && delta.at(key).is_string()) {
                    return delta.at(key).get<std::string>();
                }
            }
            return {};
        };
        std::string reasoning = reasoning_fragment();
        if (!reasoning.empty()) {
            // Same open-a-new-block rule as text after tool calls: reasoning
            // -> text -> reasoning must stay three blocks in arrival order,
            // not fuse into one.
            if (blocks_.empty() || blocks_.back().kind != Block::Kind::Thinking) {
                // Same DoS class llm::kMaxStreamBlocks exists to close for tool_calls: a
                // backend alternating 1-byte content/reasoning_content deltas
                // flips Block::Kind every frame, opening a new Block each
                // time with no delta content large enough to trip
                // Accumulate()'s content budget on its own.
                if (blocks_.size() >= llm::kMaxStreamBlocks) {
                    throw OpenAiParseError("stream opened an implausible number of content blocks");
                }
                blocks_.push_back(Block{Block::Kind::Thinking, {}, {}, {}, {}, {}, false});
            }
            Accumulate(accumulated_bytes_, reasoning.size());
            blocks_.back().text += reasoning;
            const int block_index = static_cast<int>(blocks_.size()) - 1;
            if (sink && !sink(llm::ThinkingDeltaEvent{block_index, reasoning})) return false;
        }

        if (delta.contains("content") && !delta.at("content").is_null()) {
            std::string text = delta.at("content").get<std::string>();
            // An empty delta carries nothing: opening (or appending to) a
            // block for it produced a phantom empty TextBlock in the response,
            // unlike anthropic's !text.empty() guard.
            if (!text.empty()) {
                // Text arriving AFTER tool calls started opens a NEW text block
                // instead of appending to the first: the single text_block_index_
                // reordered content on backends that narrate between calls, and
                // re-emitted a TextDelta for a block the client had already seen
                // closed. The unified model carries multi-block content natively.
                if (!text_block_index_ || blocks_.back().kind != Block::Kind::Text) {
                    if (blocks_.size() >= llm::kMaxStreamBlocks) {
                        throw OpenAiParseError(
                            "stream opened an implausible number of content blocks");
                    }
                    text_block_index_ = blocks_.size();
                    blocks_.push_back(Block{Block::Kind::Text, {}, {}, {}, {}, {}, false});
                }
                Accumulate(accumulated_bytes_, text.size());
                blocks_[*text_block_index_].text += text;
                if (sink && !sink(llm::TextDeltaEvent{static_cast<int>(*text_block_index_), text}))
                    return false;
            }
        }

        if (delta.contains("tool_calls")) {
            for (const auto& tool_call_delta : delta.at("tool_calls")) {
                // Present-but-NULL fields must be tolerated exactly like
                // absent ones: a proxy emitting `"index": null` (or null
                // id/name/arguments) is a keepalive-shaped quirk, not a
                // malformed chunk -- and the resulting ParseError is
                // NON-retryable, so null-tolerance here is the difference
                // between a dropped keepalive and a failed turn.
                int openai_index = 0;
                if (tool_call_delta.contains("index") && tool_call_delta.at("index").is_number()) {
                    openai_index = tool_call_delta.at("index").get<int>();
                }
                auto it = tool_call_indices_.find(openai_index);
                if (it == tool_call_indices_.end()) {
                    if (blocks_.size() >= llm::kMaxStreamBlocks) {
                        throw OpenAiParseError(
                            "stream event named an implausible tool_call index (" +
                            std::to_string(openai_index) + ")");
                    }
                    std::size_t block_index = blocks_.size();
                    blocks_.push_back(Block{Block::Kind::ToolCall, {}, {}, {}, {}, {}, false});
                    it = tool_call_indices_.emplace(openai_index, block_index).first;
                }
                Block& block = blocks_[it->second];

                if (tool_call_delta.contains("id") && tool_call_delta.at("id").is_string()) {
                    block.id = tool_call_delta.at("id").get<std::string>();
                }
                std::string arguments_fragment;
                if (tool_call_delta.contains("function")) {
                    const auto& function = tool_call_delta.at("function");
                    if (function.contains("name") && function.at("name").is_string()) {
                        block.name = function.at("name").get<std::string>();
                    }
                    if (function.contains("arguments") &&
                        function.at("arguments").is_string()) {
                        arguments_fragment = function.at("arguments").get<std::string>();
                        Accumulate(accumulated_bytes_, arguments_fragment.size());
                        block.arguments_json += arguments_fragment;
                    }
                }

                const bool just_started =
                    !block.started && !block.id.empty() && !block.name.empty();
                if (just_started) {
                    block.started = true;
                    if (sink &&
                        !sink(llm::ToolUseStartEvent{static_cast<int>(it->second), block.id, block.name}))
                        return false;
                }
                if (block.started) {
                    // Fragments that arrived BEFORE the startable delta were
                    // buffered (the sink had no block to attach them to);
                    // flush them ahead of this delta's own fragment so the
                    // streamed tool input is the complete arguments text.
                    std::string to_emit;
                    if (just_started) {
                        to_emit = std::move(block.pre_start_arguments);
                        block.pre_start_arguments.clear();
                    }
                    if (!arguments_fragment.empty()) to_emit += arguments_fragment;
                    if (!to_emit.empty() && sink &&
                        !sink(llm::ToolUseInputDeltaEvent{static_cast<int>(it->second), to_emit}))
                        return false;
                } else if (!arguments_fragment.empty()) {
                    block.pre_start_arguments += arguments_fragment;
                }
            }
        }

        if (!finished_ && choice.contains("finish_reason") && !choice.at("finish_reason").is_null()) {
            finished_ = true;
            completed_ = true;
            for (std::size_t i = 0; i < blocks_.size(); ++i) {
                // Thinking blocks open only on a non-empty fragment (like
                // Text), so they are always closable content.
                if (blocks_[i].kind == Block::Kind::Text ||
                    blocks_[i].kind == Block::Kind::Thinking || blocks_[i].started) {
                    if (sink && !sink(llm::ContentBlockStopEvent{static_cast<int>(i)})) return false;
                }
            }
            stop_reason_ = MapFinishReason(choice.at("finish_reason").get<std::string>());
            if (sink && !sink(llm::MessageDeltaEvent{stop_reason_})) return false;
        }
    }

    if (chunk.contains("usage") && !chunk.at("usage").is_null()) {
        usage_ = ParseUsage(chunk.at("usage"));
        if (sink && !sink(llm::UsageEvent{usage_})) return false;
    }

    return true;
}

void OpenAiStreamReassembler::CloseOpenBlocks(const llm::StreamEventSink& sink) {
    // A stream that ended without a finish_reason -- an idle timeout, a dropped
    // connection, a sink that cancelled -- left every block it had announced
    // permanently open: the client saw MessageStart and TextDelta/ToolUseStart
    // and never the matching ContentBlockStop. That is the same failure the
    // tool-lifecycle rule already names ("a client that showed a tool as started
    // must always see it finish"), applied to content blocks.
    //
    // Idempotent against the finish_reason path via finished_, so a normal
    // stream closes its blocks exactly once.
    if (finished_) return;
    finished_ = true;
    for (std::size_t i = 0; i < blocks_.size(); ++i) {
        if (blocks_[i].kind == Block::Kind::Text ||
            blocks_[i].kind == Block::Kind::Thinking || blocks_[i].started) {
            if (sink && !sink(llm::ContentBlockStopEvent{static_cast<int>(i)})) return;
        }
    }
}

llm::ChatResponse OpenAiStreamReassembler::Finalize(bool tolerate_malformed_tool_calls) const {
    llm::ChatResponse response;
    response.id = id_;
    response.model = model_;
    response.stop_reason = stop_reason_;
    response.usage = usage_;

    for (const auto& block : blocks_) {
        if (block.kind == Block::Kind::Text) {
            // Same guard anthropic's Finalize has: no phantom empty TextBlock
            // in the response even if one slipped past the delta-side filter.
            if (!block.text.empty()) response.content.push_back(llm::TextBlock{block.text});
        } else if (block.kind == Block::Kind::Thinking) {
            // Unsigned by construction: chat-completions carries no signature.
            // OpenAiRequestBuilder drops it from replay history, matching
            // provider-anthropic's posture on unsigned thinking.
            if (!block.text.empty()) {
                response.content.push_back(
                    llm::ThinkingBlock{block.text, std::nullopt, std::nullopt});
            }
        } else {
            // A tool call whose id or name never arrived cannot be run: it
            // reached ToolExecutor as `unknown tool: ` -- an error attributed to
            // a call the model believes it made, which is more confusing than
            // the call simply not being there. Dropping it leaves no tool_use in
            // history, so nothing is orphaned either; the loop already handles a
            // ToolUse stop reason with no blocks. Reachable whenever a stream is
            // cut mid-tool-call, which is exactly when Finalize runs.
            if (block.id.empty() || block.name.empty()) continue;
            llm::ToolUseBlock tool_use;
            tool_use.id = block.id;
            tool_use.name = block.name;
            try {
                if (!block.arguments_json.empty() &&
                    !llm::JsonDepthWithinLimit(block.arguments_json)) {
                    throw OpenAiParseError("tool-call arguments exceed the JSON nesting limit");
                }
                tool_use.input =
                    block.arguments_json.empty() ? nlohmann::json::object() : nlohmann::json::parse(block.arguments_json);
            } catch (const nlohmann::json::exception& error) {
                // Cancellation tolerance: a stream the SINK cut (the user hit
                // cancel while tool-call arguments were mid-JSON) must report
                // Cancelled, not a parse failure -- the §11.4 exit-code fix is
                // defeated otherwise, and the truncated call carries no
                // decision the transcript needed. Same posture as the empty-id
                // drop above: not there is more honest than half-there.
                if (tolerate_malformed_tool_calls) continue;
                throw OpenAiParseError(std::string("malformed tool-call arguments: ") + error.what());
            }
            response.content.push_back(std::move(tool_use));
        }
    }
    return response;
}

}  // namespace minimal::provider_openai
