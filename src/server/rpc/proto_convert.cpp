#include "minimal/rpc/proto_convert.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <variant>

#include "minimal/common/logger.hpp"

namespace minimal::rpc {
namespace {

// An integral double is narrowed back to a JSON integer -- see the header for
// why. Guarded on range as well as integrality: a double past int64's limits
// would be UB to cast, and is not an integer anyone meant to send.
bool IsExactInteger(double value) {
    if (!std::isfinite(value)) return false;
    if (value != std::trunc(value)) return false;
    // STRICTLY LESS at the top, and the asymmetry is the point.
    // static_cast<double>(INT64_MAX) is not INT64_MAX: 2^63-1 is not
    // representable, so the conversion rounds UP to exactly 2^63 -- and a `<=`
    // against that admitted 9223372036854775808.0, whose cast to int64_t is the
    // precise UB this guard was written to prevent. INT64_MIN is exactly -2^63
    // and needs no such care, so the bounds are deliberately not symmetric.
    // Reachable from the wire: any tool_use input carrying a number that large.
    constexpr double kTwoToThe63 = 9223372036854775808.0;  // -static_cast<double>(INT64_MIN)
    return value >= static_cast<double>(std::numeric_limits<std::int64_t>::min()) &&
           value < kTwoToThe63;
}

// Makes the terminal `else` of an if-constexpr chain a COMPILE ERROR rather than
// a silent no-op. Dependent on T so it only fires when that branch is actually
// instantiated.
template <typename>
constexpr bool kAlwaysFalse = false;

}  // namespace

// --- JSON <-> Struct ---------------------------------------------------------

google::protobuf::Value JsonToValue(const nlohmann::json& value) {
    google::protobuf::Value out;
    switch (value.type()) {
        case nlohmann::json::value_t::null:
            out.set_null_value(google::protobuf::NULL_VALUE);
            break;
        case nlohmann::json::value_t::boolean:
            out.set_bool_value(value.get<bool>());
            break;
        case nlohmann::json::value_t::number_integer:
            out.set_number_value(static_cast<double>(value.get<std::int64_t>()));
            break;
        case nlohmann::json::value_t::number_unsigned:
            out.set_number_value(static_cast<double>(value.get<std::uint64_t>()));
            break;
        case nlohmann::json::value_t::number_float:
            out.set_number_value(value.get<double>());
            break;
        case nlohmann::json::value_t::string:
            out.set_string_value(value.get<std::string>());
            break;
        case nlohmann::json::value_t::array: {
            auto* list = out.mutable_list_value();
            for (const auto& element : value) {
                *list->add_values() = JsonToValue(element);
            }
            break;
        }
        case nlohmann::json::value_t::object: {
            auto* fields = out.mutable_struct_value()->mutable_fields();
            for (const auto& [key, element] : value.items()) {
                (*fields)[key] = JsonToValue(element);
            }
            break;
        }
        // Binary has no JSON (and so no Struct) representation at all; it cannot
        // arrive from a parsed LLM response. Discarded rather than approximated.
        case nlohmann::json::value_t::binary:
        case nlohmann::json::value_t::discarded:
            out.set_null_value(google::protobuf::NULL_VALUE);
            break;
    }
    return out;
}

nlohmann::json ValueToJson(const google::protobuf::Value& value) {
    switch (value.kind_case()) {
        case google::protobuf::Value::kNullValue:
            return nullptr;
        case google::protobuf::Value::kNumberValue: {
            const double number = value.number_value();
            if (IsExactInteger(number)) return static_cast<std::int64_t>(number);
            return number;
        }
        case google::protobuf::Value::kStringValue:
            return value.string_value();
        case google::protobuf::Value::kBoolValue:
            return value.bool_value();
        case google::protobuf::Value::kStructValue: {
            nlohmann::json out = nlohmann::json::object();
            for (const auto& [key, field] : value.struct_value().fields()) {
                out[key] = ValueToJson(field);
            }
            return out;
        }
        case google::protobuf::Value::kListValue: {
            nlohmann::json out = nlohmann::json::array();
            for (const auto& element : value.list_value().values()) {
                out.push_back(ValueToJson(element));
            }
            return out;
        }
        case google::protobuf::Value::KIND_NOT_SET:
            break;
    }
    return nullptr;
}

google::protobuf::Struct JsonToStruct(const nlohmann::json& value) {
    google::protobuf::Struct out;
    if (!value.is_object()) return out;
    auto* fields = out.mutable_fields();
    for (const auto& [key, element] : value.items()) {
        (*fields)[key] = JsonToValue(element);
    }
    return out;
}

nlohmann::json StructToJson(const google::protobuf::Struct& value) {
    nlohmann::json out = nlohmann::json::object();
    for (const auto& [key, field] : value.fields()) {
        out[key] = ValueToJson(field);
    }
    return out;
}

// --- Enums -------------------------------------------------------------------
//
// The domain -> proto switches deliberately carry no `default:`. -Wswitch (on
// via -Wall) then turns "added an enumerator, forgot the mapping" into a compile
// error rather than a silent UNSPECIFIED on the wire.

v1::StopReason ToProto(llm::StopReason value) {
    switch (value) {
        case llm::StopReason::EndTurn:       return v1::STOP_REASON_END_TURN;
        case llm::StopReason::ToolUse:       return v1::STOP_REASON_TOOL_USE;
        case llm::StopReason::MaxTokens:     return v1::STOP_REASON_MAX_TOKENS;
        case llm::StopReason::StopSequence:  return v1::STOP_REASON_STOP_SEQUENCE;
        case llm::StopReason::ContentFilter: return v1::STOP_REASON_CONTENT_FILTER;
        case llm::StopReason::Refusal:       return v1::STOP_REASON_REFUSAL;
        case llm::StopReason::Cancelled:     return v1::STOP_REASON_CANCELLED;
        case llm::StopReason::Paused:        return v1::STOP_REASON_PAUSED;
        case llm::StopReason::Unknown:       return v1::STOP_REASON_UNSPECIFIED;
    }
    return v1::STOP_REASON_UNSPECIFIED;
}

llm::StopReason FromProto(v1::StopReason value) {
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

v1::Role ToProto(llm::Role value) {
    switch (value) {
        case llm::Role::User:      return v1::ROLE_USER;
        case llm::Role::Assistant: return v1::ROLE_ASSISTANT;
    }
    return v1::ROLE_UNSPECIFIED;
}

llm::Role FromProto(v1::Role value) {
    switch (value) {
        case v1::ROLE_ASSISTANT: return llm::Role::Assistant;
        // User is the safer default for an unspecified role: an unrecognised
        // message is treated as untrusted input rather than as something the
        // model is taken to have said.
        default:                 return llm::Role::User;
    }
}

v1::ToolCategory ToProto(tools::ToolCategory value) {
    switch (value) {
        case tools::ToolCategory::ReadOnly:  return v1::TOOL_CATEGORY_READ_ONLY;
        case tools::ToolCategory::FileWrite: return v1::TOOL_CATEGORY_FILE_WRITE;
        case tools::ToolCategory::Execute:   return v1::TOOL_CATEGORY_EXECUTE;
    }
    return v1::TOOL_CATEGORY_UNSPECIFIED;
}

tools::ToolCategory FromProto(v1::ToolCategory value) {
    switch (value) {
        case v1::TOOL_CATEGORY_READ_ONLY:  return tools::ToolCategory::ReadOnly;
        case v1::TOOL_CATEGORY_FILE_WRITE: return tools::ToolCategory::FileWrite;
        // Execute is the most-restricted category, so it is what an unknown
        // value must fall back to.
        default:                           return tools::ToolCategory::Execute;
    }
}

v1::LlmErrorCategory ToProto(llm::LlmErrorCategory value) {
    switch (value) {
        case llm::LlmErrorCategory::Transport:   return v1::LLM_ERROR_CATEGORY_TRANSPORT;
        case llm::LlmErrorCategory::RateLimit:   return v1::LLM_ERROR_CATEGORY_RATE_LIMIT;
        case llm::LlmErrorCategory::ServerError: return v1::LLM_ERROR_CATEGORY_SERVER_ERROR;
        case llm::LlmErrorCategory::AuthError:   return v1::LLM_ERROR_CATEGORY_AUTH_ERROR;
        case llm::LlmErrorCategory::RequestError: return v1::LLM_ERROR_CATEGORY_REQUEST_ERROR;
        case llm::LlmErrorCategory::ParseError:  return v1::LLM_ERROR_CATEGORY_PARSE_ERROR;
    }
    return v1::LLM_ERROR_CATEGORY_UNSPECIFIED;
}

llm::LlmErrorCategory FromProto(v1::LlmErrorCategory value) {
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

v1::PermissionDecision ToProto(tools::PermissionDecision value) {
    switch (value) {
        case tools::PermissionDecision::Allow: return v1::PERMISSION_DECISION_ALLOW;
        case tools::PermissionDecision::Deny:  return v1::PERMISSION_DECISION_DENY;
    }
    return v1::PERMISSION_DECISION_DENY;
}

tools::PermissionDecision FromProto(v1::PermissionDecision value) {
    // Only an explicit ALLOW allows. UNSPECIFIED -- a client that answered with a
    // default-constructed message -- denies, because the request is the model
    // asking to act and silence is not consent.
    return value == v1::PERMISSION_DECISION_ALLOW ? tools::PermissionDecision::Allow
                                                  : tools::PermissionDecision::Deny;
}

v1::PermissionRunMode ToProto(tools::PermissionRunMode value) {
    switch (value) {
        case tools::PermissionRunMode::AllowOnly: return v1::PERMISSION_RUN_MODE_ALLOW_ONLY;
        case tools::PermissionRunMode::Normal:    return v1::PERMISSION_RUN_MODE_NORMAL;
        case tools::PermissionRunMode::Bypass:    return v1::PERMISSION_RUN_MODE_BYPASS;
    }
    return v1::PERMISSION_RUN_MODE_UNSPECIFIED;
}

std::optional<tools::PermissionRunMode> FromProto(v1::PermissionRunMode value) {
    switch (value) {
        case v1::PERMISSION_RUN_MODE_ALLOW_ONLY: return tools::PermissionRunMode::AllowOnly;
        case v1::PERMISSION_RUN_MODE_NORMAL:     return tools::PermissionRunMode::Normal;
        case v1::PERMISSION_RUN_MODE_BYPASS:     return tools::PermissionRunMode::Bypass;
        default:                                 return std::nullopt;
    }
}

// --- Messages and content blocks ---------------------------------------------

v1::Usage ToProto(const llm::Usage& value) {
    v1::Usage out;
    out.set_input_tokens(value.input_tokens);
    out.set_output_tokens(value.output_tokens);
    out.set_cache_read_input_tokens(value.cache_read_input_tokens);
    out.set_cache_creation_input_tokens(value.cache_creation_input_tokens);
    return out;
}

llm::Usage FromProto(const v1::Usage& value) {
    llm::Usage out;
    out.input_tokens = value.input_tokens();
    out.output_tokens = value.output_tokens();
    out.cache_read_input_tokens = value.cache_read_input_tokens();
    out.cache_creation_input_tokens = value.cache_creation_input_tokens();
    return out;
}

v1::ContentBlock ToProto(const llm::ContentBlock& block) {
    v1::ContentBlock out;
    // Dispatched by alternative type, never by variant index: the domain variant
    // and the proto oneof are declared in different orders, so index arithmetic
    // would compile and silently mislabel every block.
    std::visit(
        [&out](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, llm::TextBlock>) {
                out.mutable_text()->set_text(value.text);
            } else if constexpr (std::is_same_v<T, llm::ThinkingBlock>) {
                auto* thinking = out.mutable_thinking();
                thinking->set_text(value.text);
                if (value.signature) thinking->set_signature(*value.signature);
                if (value.redacted_data) thinking->set_data(*value.redacted_data);
            } else if constexpr (std::is_same_v<T, llm::ToolUseBlock>) {
                auto* tool_use = out.mutable_tool_use();
                tool_use->set_id(value.id);
                tool_use->set_name(value.name);
                *tool_use->mutable_input() = JsonToStruct(value.input);
            } else if constexpr (std::is_same_v<T, llm::ToolResultBlock>) {
                auto* result = out.mutable_tool_result();
                result->set_tool_use_id(value.tool_use_id);
                result->set_is_error(value.is_error);
                for (const auto& inner : value.content) {
                    // ToolResultContentBlock is variant<TextBlock> today, so this
                    // visit has one arm; it is a visit rather than a get<> so that
                    // widening it stays a compile error here.
                    std::visit(
                        [result](const llm::TextBlock& text) {
                            result->add_content()->mutable_text()->set_text(text.text);
                        },
                        inner);
                }
                if (!value.display.is_null()) {
                    result->set_display_json(value.display.dump(
                        -1, ' ', false, nlohmann::json::error_handler_t::replace));
                }
            } else {
                // THE TOTALITY GUARANTEE THE HEADER CLAIMS, made real. The enum
                // switches get it from -Wswitch; this chain had no terminal
                // else, so adding a ContentBlock alternative compiled silently,
                // serialized as BLOCK_NOT_SET, and was then DROPPED by the
                // reverse direction -- silently deleted transcript content,
                // which is exactly what the header says cannot happen.
                static_assert(kAlwaysFalse<T>, "ToProto: unhandled ContentBlock alternative");
            }
        },
        block);
    return out;
}

std::optional<llm::ContentBlock> FromProto(const v1::ContentBlock& block) {
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

v1::Message ToProto(const llm::Message& message) {
    v1::Message out;
    out.set_role(ToProto(message.role));
    for (const auto& block : message.content) {
        *out.add_content() = ToProto(block);
    }
    return out;
}

llm::Message FromProto(const v1::Message& message) {
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

// --- Stream events -----------------------------------------------------------

v1::StreamEvent ToProto(const llm::StreamEvent& event) {
    v1::StreamEvent out;
    std::visit(
        [&out](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, llm::MessageStartEvent>) {
                auto* start = out.mutable_message_start();
                start->set_id(value.id);
                start->set_model(value.model);
            } else if constexpr (std::is_same_v<T, llm::TextDeltaEvent>) {
                auto* delta = out.mutable_text_delta();
                delta->set_block_index(value.block_index);
                delta->set_text(value.text);
            } else if constexpr (std::is_same_v<T, llm::ToolUseStartEvent>) {
                auto* start = out.mutable_tool_use_start();
                start->set_block_index(value.block_index);
                start->set_id(value.id);
                start->set_name(value.name);
            } else if constexpr (std::is_same_v<T, llm::ToolUseInputDeltaEvent>) {
                auto* delta = out.mutable_tool_use_input_delta();
                delta->set_block_index(value.block_index);
                delta->set_partial_json(value.partial_json);
            } else if constexpr (std::is_same_v<T, llm::ThinkingDeltaEvent>) {
                auto* delta = out.mutable_thinking_delta();
                delta->set_block_index(value.block_index);
                delta->set_text(value.text);
            } else if constexpr (std::is_same_v<T, llm::ContentBlockStopEvent>) {
                out.mutable_content_block_stop()->set_block_index(value.block_index);
            } else if constexpr (std::is_same_v<T, llm::MessageDeltaEvent>) {
                out.mutable_message_delta()->set_stop_reason(ToProto(value.stop_reason));
            } else if constexpr (std::is_same_v<T, llm::UsageEvent>) {
                *out.mutable_usage()->mutable_usage() = ToProto(value.usage);
            } else if constexpr (std::is_same_v<T, llm::MessageStopEvent>) {
                out.mutable_message_stop();
            } else {
                // Same guarantee as the ContentBlock visit above: a new
                // StreamEvent alternative must not compile into EVENT_NOT_SET.
                static_assert(kAlwaysFalse<T>, "ToProto: unhandled StreamEvent alternative");
            }
        },
        event);
    return out;
}

std::optional<llm::StreamEvent> FromProto(const v1::StreamEvent& event) {
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

// --- Errors ------------------------------------------------------------------

v1::ErrorEvent ToProto(const llm::LlmError& error) {
    v1::ErrorEvent out;
    out.set_category(ToProto(error.category()));
    out.set_message(error.what());
    if (error.http_status()) out.set_http_status(*error.http_status());
    // Sent rather than left for the client to re-derive from the category: the
    // retry rule is the server's, and duplicating it client-side is how the two
    // drift apart.
    out.set_is_retryable(error.is_retryable());
    return out;
}

// --- Session events ------------------------------------------------------------

v1::ServerEvent ToProto(const std::string& session_id, const agent::SessionEvent& event) {
    v1::ServerEvent out;
    out.set_session_id(session_id);
    std::visit(
        [&out](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, llm::StreamEvent>) {
                *out.mutable_stream_event() = ToProto(value);
            } else if constexpr (std::is_same_v<T, agent::ToolLifecycleEvent>) {
                auto* execution = out.mutable_tool_execution();
                execution->set_tool_use_id(value.tool_use_id);
                execution->set_tool_name(value.tool_name);
                *execution->mutable_input() = JsonToStruct(value.input);
                execution->set_started(value.started);
                if (!value.started) {
                    execution->set_is_error(value.is_error);
                    execution->set_duration_ms(value.duration.count());
                }
                execution->set_started_at_unix_ms(value.started_at_unix_ms);
            } else if constexpr (std::is_same_v<T, tools::BackgroundTaskEvent>) {
                auto* message = out.mutable_background_task();
                message->set_task_id(value.task_id);
                message->set_tool_use_id(value.tool_use_id);
                message->set_command(value.command);
                message->set_pid(value.pid);
                switch (value.state) {
                    case tools::BackgroundTaskEvent::State::kStarted:
                        message->set_state(v1::BackgroundTaskEvent::STARTED);
                        break;
                    case tools::BackgroundTaskEvent::State::kCompleted:
                        message->set_state(v1::BackgroundTaskEvent::COMPLETED);
                        break;
                    case tools::BackgroundTaskEvent::State::kFailed:
                        message->set_state(v1::BackgroundTaskEvent::FAILED);
                        break;
                    case tools::BackgroundTaskEvent::State::kTimedOut:
                        message->set_state(v1::BackgroundTaskEvent::TIMED_OUT);
                        break;
                    case tools::BackgroundTaskEvent::State::kKilled:
                        message->set_state(v1::BackgroundTaskEvent::KILLED);
                        break;
                }
            } else if constexpr (std::is_same_v<T, llm::LlmError>) {
                *out.mutable_error() = ToProto(value);
            } else if constexpr (std::is_same_v<T, agent::SessionCommandResult>) {
                auto* result = out.mutable_command_result();
                result->set_request_id(value.request_id);
                FillProto(*result, value.result);
            } else if constexpr (std::is_same_v<T, agent::ToolOutputDelta>) {
                auto* delta = out.mutable_tool_output_delta();
                delta->set_tool_use_id(value.tool_use_id);
                delta->set_chunk(value.chunk);
            } else if constexpr (std::is_same_v<T, agent::SubagentStarted>) {
                auto* started = out.mutable_subagent_started();
                started->set_child_session_id(value.child_session_id);
                started->set_task(value.task);
                started->set_blocking(value.blocking);
            } else if constexpr (std::is_same_v<T, agent::SessionMetadataChanged>) {
                auto* metadata = out.mutable_session_metadata();
                metadata->set_session_id(out.session_id());
                metadata->set_name(value.name);
                // `partial` mirrors today's two name-only emission sites
                // (SetName, EmitDefaultNameIfComputed): the other fields are left
                // at their proto default rather than sent as zero values that
                // could be mistaken for an authoritative update.
                if (!value.partial) {
                    metadata->set_agent_preset(value.agent_preset);
                    metadata->set_model(value.model);
                    metadata->set_permission_mode(value.permission_mode);
                    metadata->set_context_window_tokens(value.context_window_tokens);
                    metadata->set_paused(value.paused);
                }
            } else if constexpr (std::is_same_v<T, agent::SessionEnded>) {
                out.mutable_session_ended();
            } else if constexpr (std::is_same_v<T, agent::SessionRewound>) {
                auto* rewound = out.mutable_session_rewound();
                rewound->set_truncated_to_seq(value.truncated_to_seq);
                rewound->set_active_last_seq(value.active_last_seq);
            } else if constexpr (std::is_same_v<T, agent::TurnCompleted>) {
                auto* completed = out.mutable_turn_completed();
                completed->set_stop_reason(ToProto(value.stop_reason));
                *completed->mutable_usage() = ToProto(value.usage);
                completed->set_hit_max_turns(value.hit_max_turns);
                completed->set_usage_estimated(value.usage_estimated);
            } else if constexpr (std::is_same_v<T, agent::SessionError>) {
                auto* error = out.mutable_error();
                error->set_category(v1::LLM_ERROR_CATEGORY_UNSPECIFIED);
                error->set_message(value.message);
                error->set_is_retryable(false);
            } else if constexpr (std::is_same_v<T, agent::TurnQueued>) {
                auto* queued = out.mutable_turn_queued();
                queued->set_turn_id(value.turn_id);
                queued->set_text(value.text);
                queued->set_steered(value.steered);
            } else if constexpr (std::is_same_v<T, agent::TurnCanceled>) {
                auto* canceled = out.mutable_turn_canceled();
                canceled->set_turn_id(value.turn_id);
                canceled->set_removed(value.removed);
            } else {
                // Same guarantee as every other domain->wire visit in this file: a
                // new SessionEvent alternative must not compile into PAYLOAD_NOT_SET.
                static_assert(kAlwaysFalse<T>, "ToProto: unhandled SessionEvent alternative");
            }
        },
        event);
    return out;
}

// --- Permission round-trip ---------------------------------------------------

v1::PermissionRequest ToProto(const tools::PermissionRequest& request) {
    v1::PermissionRequest out;
    out.set_tool_name(request.tool_name);
    out.set_category(ToProto(request.category));
    *out.mutable_tool_input() = JsonToStruct(request.tool_input);
    // The disclosure's summary is the prompt line when there is one; the bare
    // tool name is the default Disclose() produces for a tool that has not opted
    // in, and is what request.summary already carries.
    out.set_summary(request.disclosure.summary.empty() ? request.summary
                                                       : request.disclosure.summary);
    for (const auto& offer : request.disclosure.grants) {
        auto* grant = out.add_grants();
        grant->set_key(offer.key);
        grant->set_label(offer.label);
    }
    out.set_complete(request.disclosure.complete);
    return out;
}

tools::PermissionResponse FromProto(const v1::PermissionResponse& response,
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

v1::StoredMessage ToProto(const session::StoredMessage& stored) {
    v1::StoredMessage out;
    out.set_seq(stored.seq);
    *out.mutable_message() = ToProto(stored.message);
    out.set_token_count(stored.token_count);
    out.set_created_at_unix_ms(stored.created_at_unix_ms);
    out.set_agent_preset(stored.attribution.agent_preset);
    out.set_model(stored.attribution.model);
    return out;
}

v1::SessionSummary ToProto(const session::SessionSummary& summary) {
    v1::SessionSummary out;
    out.set_session_id(summary.session_id);
    // Absent rather than empty in the domain type; the proto has no optional
    // here because "" is already the unambiguous "top-level session".
    out.set_parent_session_id(summary.parent_session_id.value_or(""));
    out.set_name(summary.name);
    out.set_project_root(summary.project_root);
    out.set_agent_preset(summary.agent_preset);
    out.set_created_at_unix_ms(summary.created_at_unix_ms);
    out.set_updated_at_unix_ms(summary.updated_at_unix_ms);
    out.set_message_count(summary.message_count);
    out.set_forked_from_session_id(summary.forked_from_session_id);
    return out;
}

commands::CommandArgs FromProto(const v1::CommandArgs& args) {
    commands::CommandArgs out;
    out.positional.assign(args.positional().begin(), args.positional().end());
    for (const auto& [key, value] : args.flags()) out.flags.emplace(key, value);
    out.raw = args.raw();
    return out;
}

void FillProto(v1::CommandResult& out, const commands::CommandResult& value) {
    out.set_status(value.status == commands::CommandResult::Status::kOk
                       ? v1::COMMAND_STATUS_OK
                       : v1::COMMAND_STATUS_ERROR);
    out.set_message(value.message);
    out.set_display(value.display == commands::CommandDisplay::kView
                        ? v1::COMMAND_DISPLAY_VIEW
                        : value.display == commands::CommandDisplay::kSilent
                              ? v1::COMMAND_DISPLAY_SILENT
                              : v1::COMMAND_DISPLAY_STATUS);
    out.set_view_hint(value.view_hint);
    out.set_submitted_turn(value.submitted_turn);
}

}  // namespace minimal::rpc
