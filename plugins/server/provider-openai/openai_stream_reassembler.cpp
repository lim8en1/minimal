#include "openai_stream_reassembler.hpp"

#include <nlohmann/json.hpp>

#include "openai_response.hpp"

namespace minimal::provider_openai {

bool OpenAiStreamReassembler::FeedFrame(std::string_view data, const llm::StreamEventSink& sink) {
    if (data == "[DONE]") {
        return !sink || sink(llm::MessageStopEvent{});
    }

    nlohmann::json chunk;
    try {
        chunk = nlohmann::json::parse(data);
    } catch (const nlohmann::json::exception& error) {
        throw OpenAiParseError(std::string("malformed stream chunk: ") + error.what());
    }

    if (!message_started_) {
        id_ = chunk.value("id", "");
        model_ = chunk.value("model", "");
        message_started_ = true;
        if (sink && !sink(llm::MessageStartEvent{id_, model_})) return false;
    }

    if (chunk.contains("choices") && !chunk.at("choices").empty()) {
        const auto& choice = chunk.at("choices").at(0);
        nlohmann::json delta = choice.value("delta", nlohmann::json::object());

        if (delta.contains("content") && !delta.at("content").is_null()) {
            std::string text = delta.at("content").get<std::string>();
            if (!text_block_index_) {
                text_block_index_ = blocks_.size();
                blocks_.push_back(Block{Block::Kind::Text, {}, {}, {}, {}, false});
            }
            blocks_[*text_block_index_].text += text;
            if (sink && !sink(llm::TextDeltaEvent{static_cast<int>(*text_block_index_), text})) return false;
        }

        if (delta.contains("tool_calls")) {
            for (const auto& tool_call_delta : delta.at("tool_calls")) {
                int openai_index = tool_call_delta.value("index", 0);
                auto it = tool_call_indices_.find(openai_index);
                if (it == tool_call_indices_.end()) {
                    std::size_t block_index = blocks_.size();
                    blocks_.push_back(Block{Block::Kind::ToolCall, {}, {}, {}, {}, false});
                    it = tool_call_indices_.emplace(openai_index, block_index).first;
                }
                Block& block = blocks_[it->second];

                if (tool_call_delta.contains("id")) block.id = tool_call_delta.at("id").get<std::string>();
                std::string arguments_fragment;
                if (tool_call_delta.contains("function")) {
                    const auto& function = tool_call_delta.at("function");
                    if (function.contains("name")) block.name = function.at("name").get<std::string>();
                    if (function.contains("arguments")) {
                        arguments_fragment = function.at("arguments").get<std::string>();
                        block.arguments_json += arguments_fragment;
                    }
                }

                if (!block.started && !block.id.empty() && !block.name.empty()) {
                    block.started = true;
                    if (sink &&
                        !sink(llm::ToolUseStartEvent{static_cast<int>(it->second), block.id, block.name}))
                        return false;
                }
                if (block.started && !arguments_fragment.empty()) {
                    if (sink && !sink(llm::ToolUseInputDeltaEvent{static_cast<int>(it->second), arguments_fragment}))
                        return false;
                }
            }
        }

        if (!finished_ && choice.contains("finish_reason") && !choice.at("finish_reason").is_null()) {
            finished_ = true;
            for (std::size_t i = 0; i < blocks_.size(); ++i) {
                if (blocks_[i].kind == Block::Kind::Text || blocks_[i].started) {
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

llm::ChatResponse OpenAiStreamReassembler::Finalize() const {
    llm::ChatResponse response;
    response.id = id_;
    response.model = model_;
    response.stop_reason = stop_reason_;
    response.usage = usage_;

    for (const auto& block : blocks_) {
        if (block.kind == Block::Kind::Text) {
            response.content.push_back(llm::TextBlock{block.text});
        } else {
            llm::ToolUseBlock tool_use;
            tool_use.id = block.id;
            tool_use.name = block.name;
            try {
                tool_use.input =
                    block.arguments_json.empty() ? nlohmann::json::object() : nlohmann::json::parse(block.arguments_json);
            } catch (const nlohmann::json::exception& error) {
                throw OpenAiParseError(std::string("malformed tool-call arguments: ") + error.what());
            }
            response.content.push_back(std::move(tool_use));
        }
    }
    return response;
}

}  // namespace minimal::provider_openai
