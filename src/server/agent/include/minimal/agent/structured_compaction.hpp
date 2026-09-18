#pragma once

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "minimal/agent/compaction_strategy.hpp"
#include "minimal/agent/context_compactor.hpp"

namespace minimal::agent {

// One entry of the structured summary's `important` section. `name` must be a
// verbatim identifier -- a file path, tool name, MCP call, or a `topics` entry
// -- the key the continuing model can resolve without guessing; that contract
// lives in the wire schema's field description, not here, because this struct
// only carries what survived parsing.
struct ImportantItem {
    std::string name;
    std::string why;
    std::string how;  // optional at parse; omitted at render when empty
};

// The structured summary's fields, in the order RenderStructuredSummary emits
// them -- urgency for the continuing model first. This declared order IS the
// render order and the wire schema's property order; keep the three in sync.
// The only hard requirement is current_task (keystone: a summary whose task is
// wrong is unreadable however good the rest is); every list degrades to
// "section absent" when empty, per the validation policy in the .cpp.
struct StructuredSummary {
    std::string current_task;
    std::vector<std::string> done;
    std::vector<std::string> next_steps;
    std::vector<std::string> topics;
    std::vector<ImportantItem> important;
    std::vector<std::string> lessons;
    std::vector<std::string> future_tasks;
};

// kSummaryFraming + one "## Section" per non-empty field in declared order.
// `current_task` renders as a paragraph under its header; list items render as
// "- " bullets; `important` items render as "- <name> -- <why> (how: <how>)",
// the "(how: ...)" suffix omitted when `how` is empty. The output is plain
// text -- byte-shape identical in kind to the free-text head, so checkpoint,
// resume, and the wire see nothing new.
std::string RenderStructuredSummary(const StructuredSummary& summary);

// The "structured" compaction strategy's Compactor half: the same call
// compact_messages makes -- verbatim range, preset sampling, session routing
// key, cancel sink, and the refusals for blank prompt / LlmError / Cancelled /
// MaxTokens -- with the summarizer's output constrained to a forced
// `record_summary` tool call whose input is the schema below, parsed and
// rendered through StructuredSummary.
//
// Degradation policy (why this does not just refuse): a refusal re-fires next
// turn while occupancy keeps climbing toward a hard 400, and no portable
// enforcement story exists for the schema's required/minItems across OpenAI
// strict mode, Anthropic, and llama.cpp. So missing current_task renders with
// a visible "(current task: not reported)" marker and a warn rather than
// failing six good fields; non-string array items and `important` entries
// without a name are filtered with a warn; duplicate topics/important names
// are kept with a warn (the list shape makes them detectable, which a dynamic
// key object would not be). REFUSAL (nullopt) is reserved for: no tool call
// with no usable text (the free-text fallback's own refusal), and tool-call
// args that do not parse as JSON at all. A provider that ignored tool_choice
// but produced text degrades to a framed free-text summary, logged -- the
// same head shape compact_messages returns.
std::optional<CompactionResult> compact_messages_structured(
    std::span<const llm::Message> messages_to_compact, llm::LlmProvider& provider,
    const CompactionRequest& request);

// The "structured" strategy: today's cut selection (identical to "summarize" --
// where to cut is orthogonal to how to summarize) with the structured
// Compactor above. Registered by CORE in main.cpp beside "summarize", and the
// default value of settings.json's memory.compaction_strategy.
CompactionStrategy MakeStructuredCompactionStrategy();

}  // namespace minimal::agent
