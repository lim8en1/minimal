#pragma once

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "minimal/llm/message.hpp"

namespace minimal::llm {

struct ToolDefinition {
    std::string name;
    std::string description;
    nlohmann::json input_schema;
    // Grammar-constrained arguments (Anthropic `strict`, OpenAI
    // `function.strict`). The schema must then be strict-compatible: every
    // property required, additionalProperties false at every level.
    bool strict = false;
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
    // The session was paused (SetPausedEvent): the turn ended at the
    // tool-call-return boundary -- the in-flight round ran to completion and
    // its results are persisted -- and no further provider call was made.
    // Distinct from Cancelled: "suspended, will continue", not "halted".
    Paused,
    Unknown,
};

struct ChatRequest {
    std::optional<std::string> system;

    // BORROWED, not owned. The request is assembled once per tool cycle and once
    // per retry, over a history this project intends to run to 512k-1M tokens --
    // so owning it meant a full deep copy of every message and every string on
    // the hottest path there is, and then a second one when a provider copied the
    // request to normalize it. Measured at ~114 us per call on a 2 MB history,
    // which is per tool cycle, discarded immediately after serialization.
    //
    // LIFETIME CONTRACT, and it is on the caller: the referenced messages must
    // outlive the Complete/StreamComplete call. Providers serialize during the
    // call and retain nothing -- that is a requirement on implementations, not an
    // observation about the one that exists. Anything that stores a ChatRequest
    // past the call (tests recording requests for assertion) must copy the
    // messages itself and re-point the span; see tests/support/fakes.hpp.
    std::span<const Message> messages;

    // ALWAYS sorted by name. `tools` sits at the very front of the serialized
    // prefix providers cache on, and the set is rebuilt every turn, so unstable
    // ordering would silently destroy prompt-cache hits on every turn -- with no
    // symptom other than cost and latency. Built via ToolRegistry::DefinitionsFor.
    std::vector<ToolDefinition> tools;
    ToolChoiceSpec tool_choice;

    std::string model;
    std::optional<int> max_tokens;
    bool stream = false;

    // Polled by the transport while the call is in flight (at least about once
    // a second, even when no bytes arrive); returning true abandons the
    // transfer and the provider throws instead of waiting for the next event
    // or the idle timeout. Without it a cancel during a silent prefill only
    // lands when the backend finally speaks. Empty = never abort.
    std::function<bool()> should_abort;

    // The harness session id, set by the agent loop on every request it builds
    // (compaction summarizer calls included). Empty means "unknown" -- tests
    // and direct provider users -- and serializes nothing. Backends emit it
    // when it carries a routing value: provider-openai as a top-level
    // "session_id" body field, provider-anthropic as an "x-session-id" header
    // (the real API ignores custom headers; OpenRouter accepts both and uses
    // the value as its provider-sticky-routing key). Opt-out is per provider
    // entry via "parameters": {"session_id": false}; a STRING "session_id"
    // value in parameters passes through verbatim as a static routing key and
    // outranks this field -- both are the user's explicit choice. Never
    // persisted and never derived from the API key, so it carries no secret.
    std::string session_id;

    // Arbitrary sampling/generation knobs passed through verbatim from the
    // resolved providers.json entry (temperature, top_p, reasoning_effort,
    // id_slot, ...). Structural fields above always win on serialize, so a
    // misconfigured `parameters` can tweak generation but never corrupt the
    // conversation or tool-calling mechanics. A few keys are CONSUMED by the
    // provider builders instead of passing through -- "cache_control" and
    // "cache_ttl" steer prompt-cache breakpoint emission (see
    // provider-openai/openai_request.hpp and provider-anthropic) and a
    // boolean "session_id" toggles the session-id emission described above --
    // and are dropped from the wire like any other structural key.
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
