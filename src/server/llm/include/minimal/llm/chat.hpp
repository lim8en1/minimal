#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "minimal/llm/message.hpp"

namespace minimal::llm {

struct ToolDefinition {
    std::string name;
    std::string description;
    nlohmann::json input_schema;
};

enum class ToolChoiceKind { Auto, None, Required, Specific };

struct ToolChoiceSpec {
    ToolChoiceKind kind = ToolChoiceKind::Auto;
    std::optional<std::string> tool_name;  // set iff kind == Specific
};

enum class StopReason {
    EndTurn,
    ToolUse,
    MaxTokens,
    StopSequence,
    ContentFilter,
    Refusal,
    // Cancellation is expected, not exceptional, so it does NOT surface as an
    // LlmError: the provider catches its own aborted transfer and returns a
    // partial ChatResponse carrying this stop reason.
    Cancelled,
    Unknown,
};

struct ChatRequest {
    std::optional<std::string> system;
    std::vector<Message> messages;

    // ALWAYS sorted by name. `tools` sits at the very front of the serialized
    // prefix providers cache on, and the set is rebuilt every turn, so unstable
    // ordering would silently destroy prompt-cache hits on every turn -- with no
    // symptom other than cost and latency. Built via ToolRegistry::DefinitionsFor.
    std::vector<ToolDefinition> tools;
    ToolChoiceSpec tool_choice;

    std::string model;
    std::optional<int> max_tokens;
    bool stream = false;

    // Arbitrary sampling/generation knobs passed through verbatim from the
    // resolved providers.json entry (temperature, top_p, reasoning_effort,
    // id_slot, ...). Structural fields above always win on serialize, so a
    // misconfigured `parameters` can tweak generation but never corrupt the
    // conversation or tool-calling mechanics.
    nlohmann::json parameters = nlohmann::json::object();
};

struct ChatResponse {
    std::string id;
    std::string model;
    std::vector<ContentBlock> content;
    StopReason stop_reason = StopReason::Unknown;
    Usage usage;
};

}  // namespace minimal::llm
