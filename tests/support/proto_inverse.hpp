// Test-only inverses of rpc/proto_convert's ToProto overloads. Production only
// ever converts domain -> wire for these types (the wire -> domain direction
// for messages and stream events has no caller), so the inverses live here to
// let the round-trip tests prove ToProto is lossless without shipping unused
// code in the server.
#pragma once

#include <optional>

#include "minimal/rpc/proto_convert.hpp"

namespace minimal::rpc {

inline llm::StopReason FromProto(v1::StopReason value) {
    switch (value) {
        case v1::STOP_REASON_END_TURN:       return llm::StopReason::EndTurn;
        case v1::STOP_REASON_TOOL_USE:       return llm::StopReason::ToolUse;
        case v1::STOP_REASON_MAX_TOKENS:     return llm::StopReason::MaxTokens;
        case v1::STOP_REASON_STOP_SEQUENCE:  return llm::StopReason::StopSequence;
        case v1::STOP_REASON_CONTENT_FILTER: return llm::StopReason::ContentFilter;
        case v1::STOP_REASON_REFUSAL:        return llm::StopReason::Refusal;
        case v1::STOP_REASON_CANCELLED:      return llm::StopReason::Cancelled;
        case v1::STOP_REASON_PAUSED:         return llm::StopReason::Paused;
        default:                             return llm::StopReason::Unknown;
    }
}

inline llm::Role FromProto(v1::Role value) {
    switch (value) {
        case v1::ROLE_ASSISTANT: return llm::Role::Assistant;
        // User is the safer default for an unspecified role: an unrecognised
        // message is treated as untrusted input rather than as something the
        // model is taken to have said.
        default:                 return llm::Role::User;
    }
}

inline tools::ToolCategory FromProto(v1::ToolCategory value) {
    switch (value) {
        case v1::TOOL_CATEGORY_READ_ONLY:  return tools::ToolCategory::ReadOnly;
        case v1::TOOL_CATEGORY_FILE_WRITE: return tools::ToolCategory::FileWrite;
        // Execute is the most-restricted category, so it is what an unknown
        // value must fall back to.
        default:                           return tools::ToolCategory::Execute;
    }
}

inline llm::LlmErrorCategory FromProto(v1::LlmErrorCategory value) {
    switch (value) {
        case v1::LLM_ERROR_CATEGORY_TRANSPORT:    return llm::LlmErrorCategory::Transport;
        case v1::LLM_ERROR_CATEGORY_RATE_LIMIT:   return llm::LlmErrorCategory::RateLimit;
        case v1::LLM_ERROR_CATEGORY_SERVER_ERROR: return llm::LlmErrorCategory::ServerError;
        case v1::LLM_ERROR_CATEGORY_AUTH_ERROR:   return llm::LlmErrorCategory::AuthError;
        case v1::LLM_ERROR_CATEGORY_PARSE_ERROR:  return llm::LlmErrorCategory::ParseError;
        // RequestError is the non-retryable default: an error we cannot classify
        // must not be retried on the assumption that it might succeed.
        default:                                  return llm::LlmErrorCategory::RequestError;
    }
}

inline llm::Usage FromProto(const v1::Usage& value) {
    llm::Usage out;
    out.input_tokens = value.input_tokens();
    out.output_tokens = value.output_tokens();
    out.cache_read_input_tokens = value.cache_read_input_tokens();
    out.cache_creation_input_tokens = value.cache_creation_input_tokens();
    return out;
}

inline std::optional<llm::ContentBlock> FromProto(const v1::ContentBlock& block) {
    switch (block.block_case()) {
        case v1::ContentBlock::kText:
            return llm::ContentBlock{llm::TextBlock{block.text().text()}};
        case v1::ContentBlock::kThinking: {
            llm::ThinkingBlock thinking;
            thinking.text = block.thinking().text();
            if (block.thinking().has_signature()) thinking.signature = block.thinking().signature();
            if (block.thinking().has_data()) thinking.redacted_data = block.thinking().data();
            return llm::ContentBlock{std::move(thinking)};
        }
        case v1::ContentBlock::kToolUse:
            return llm::ContentBlock{llm::ToolUseBlock{block.tool_use().id(),
                                                       block.tool_use().name(),
                                                       StructToJson(block.tool_use().input())}};
        case v1::ContentBlock::kToolResult: {
            llm::ToolResultBlock result;
            result.tool_use_id = block.tool_result().tool_use_id();
            result.is_error = block.tool_result().is_error();
            // The wire type is recursive; the domain type is deliberately not,
            // because no wire format nests a tool_use inside a tool_result. So a
            // non-text child is malformed input, not a shape we lost: drop it
            // with a reason rather than inventing a representation for it.
            for (const auto& inner : block.tool_result().content()) {
                if (inner.block_case() == v1::ContentBlock::kText) {
                    result.content.push_back(llm::TextBlock{inner.text().text()});
                } else {
                    common::DefaultLogger().warn(
                        "proto_convert: dropped non-text block (case {}) inside tool_result {}",
                        static_cast<int>(inner.block_case()), result.tool_use_id);
                }
            }
            if (block.tool_result().has_display_json()) {
                auto display = nlohmann::json::parse(block.tool_result().display_json(),
                                                     nullptr, /*allow_exceptions=*/false);
                if (display.is_object()) {
                    result.display = std::move(display);
                } else {
                    common::DefaultLogger().warn(
                        "proto_convert: dropped malformed display_json on tool_result {}",
                        result.tool_use_id);
                }
            }
            return llm::ContentBlock{std::move(result)};
        }
        case v1::ContentBlock::BLOCK_NOT_SET:
            break;
    }
    return std::nullopt;
}

inline llm::Message FromProto(const v1::Message& message) {
    llm::Message out;
    out.role = FromProto(message.role());
    for (const auto& block : message.content()) {
        if (auto converted = FromProto(block)) {
            out.content.push_back(std::move(*converted));
        } else {
            // nullopt here covers BOTH the unset case and an UNKNOWN future
            // block case (the inner switch matches neither); the old message
            // claimed "unset" for every drop.
            common::DefaultLogger().warn(
                "proto_convert: dropped content block (unset or unknown oneof case {})",
                static_cast<int>(block.block_case()));
        }
    }
    return out;
}

inline std::optional<llm::StreamEvent> FromProto(const v1::StreamEvent& event) {
    switch (event.event_case()) {
        case v1::StreamEvent::kMessageStart:
            return llm::StreamEvent{
                llm::MessageStartEvent{event.message_start().id(), event.message_start().model()}};
        case v1::StreamEvent::kTextDelta:
            return llm::StreamEvent{llm::TextDeltaEvent{event.text_delta().block_index(),
                                                        event.text_delta().text()}};
        case v1::StreamEvent::kToolUseStart:
            return llm::StreamEvent{llm::ToolUseStartEvent{event.tool_use_start().block_index(),
                                                           event.tool_use_start().id(),
                                                           event.tool_use_start().name()}};
        case v1::StreamEvent::kToolUseInputDelta:
            return llm::StreamEvent{
                llm::ToolUseInputDeltaEvent{event.tool_use_input_delta().block_index(),
                                            event.tool_use_input_delta().partial_json()}};
        case v1::StreamEvent::kThinkingDelta:
            return llm::StreamEvent{llm::ThinkingDeltaEvent{event.thinking_delta().block_index(),
                                                            event.thinking_delta().text()}};
        case v1::StreamEvent::kContentBlockStop:
            return llm::StreamEvent{
                llm::ContentBlockStopEvent{event.content_block_stop().block_index()}};
        case v1::StreamEvent::kMessageDelta:
            return llm::StreamEvent{
                llm::MessageDeltaEvent{FromProto(event.message_delta().stop_reason())}};
        case v1::StreamEvent::kUsage:
            return llm::StreamEvent{llm::UsageEvent{FromProto(event.usage().usage())}};
        case v1::StreamEvent::kMessageStop:
            return llm::StreamEvent{llm::MessageStopEvent{}};
        case v1::StreamEvent::EVENT_NOT_SET:
            break;
    }
    return std::nullopt;
}

inline tools::PermissionResponse FromProto(const v1::PermissionResponse& response,
                                    const tools::PermissionDisclosure& offered) {
    tools::PermissionResponse out;
    out.decision = FromProto(response.decision());
    out.deny_message = response.deny_message();
    out.stop_turn = response.stop_turn();
    // A grant key is only meaningful as the answer to something we asked. Taking
    // arbitrary keys off the wire would let a client mint permissions for
    // binaries no prompt ever showed a human -- the exact failure the disclosure
    // exists to prevent.
    for (const auto& key : response.granted_keys()) {
        const bool was_offered =
            std::any_of(offered.grants.begin(), offered.grants.end(),
                        [&key](const tools::GrantOffer& offer) { return offer.key == key; });
        if (was_offered) {
            out.granted_keys.push_back(key);
        } else {
            common::DefaultLogger().warn("proto_convert: ignoring ungranted key '{}'", key);
        }
    }
    return out;
}

}  // namespace minimal::rpc
