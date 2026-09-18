#include "minimal/agent/structured_compaction.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

#include "minimal/agent/compaction_boundary.hpp"
#include "minimal/agent/compaction_strategy.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/llm/chat.hpp"

namespace minimal::agent {
namespace {

// The forced tool call whose input carries the structured summary. The schema's
// description text is the prompt: every rule the free-text compression prompt
// states that survives structuring lives on the field it governs, where a
// provider synthesizing tool context will see it next to the field it shapes.
// `request.system` stays the preset's compression prompt, so a manual
// /compact <name> override keeps steering section emphasis.
constexpr std::string_view kRecordSummaryTool = "record_summary";

constexpr std::string_view kRecordSummaryDescription =
    "Record the structured summary replacing the conversation prefix. Every "
    "identifier -- file paths, tool names, commands, MCP calls -- must be "
    "verbatim; a paraphrased identifier is a broken one. Be dense and factual: "
    "this summary replaces the original messages.";

// One source of truth for the wire property order and the render order; the
// parser walks the declared fields of StructuredSummary directly, so this is
// documentation, not a lookup table.
constexpr std::array<std::string_view, 7> kFields = {
    "current_task", "done", "next_steps", "topics", "important", "lessons", "future_tasks"};

nlohmann::json FieldSchema(const char* description) {
    return nlohmann::json{{"type", "string"}, {"description", description}};
}

nlohmann::json StringArraySchema(const char* description) {
    return nlohmann::json{{"type", "array"},
                          {"items", nlohmann::json{{"type", "string"}}},
                          {"description", description}};
}

// Built once: the schema is identical on every compaction attempt, and
// compaction is infrequent enough that this is about clarity, not perf.
const nlohmann::json& RecordSummarySchema() {
    static const nlohmann::json schema{
        {"type", "object"},
        // The only hard requirement. required/minItems are prompt intentions
        // at runtime -- enforcement varies by backend (OpenAI strict mode
        // honors them, Anthropic treats input_schema as a hint, llama.cpp
        // varies) -- and the .cpp's parse degrades rather than refusing on
        // their absence for exactly that reason. They stay declared for the
        // backends that do enforce.
        {"required", nlohmann::json::array({"current_task"})},
        {"additionalProperties", false},
        {"properties",
         nlohmann::json{
             {"current_task",
              FieldSchema("Brief description of what the agent was doing "
                          "before compaction and why: the task itself, what "
                          "was done so far, and what is currently failing or "
                          "waiting on what. The summary's keystone.")},
             {"done",
              StringArraySchema("Concrete completed work, each item factual: "
                                "file paths verbatim, commands with their "
                                "outcomes, decisions made and why. Oldest to "
                                "newest.")},
             {"next_steps",
              StringArraySchema("What to do next, in order; may reference "
                                "files, topics, or tools by verbatim name. "
                                "Empty ONLY when the task is complete or the "
                                "agent is awaiting the user.")},
             {"topics",
              nlohmann::json{{"type", "array"},
                             {"items", nlohmann::json{{"type", "string"}}},
                             {"minItems", 1},
                             {"description",
                              "One sentence per distinct topic the session "
                              "touched. These are the index keys `important` "
                              "hangs off -- list a topic only if the "
                              "continuing session may need to find its way "
                              "back to it."}}},
             {"important",
              nlohmann::json{
                  {"type", "array"},
                  {"items",
                   nlohmann::json{
                       {"type", "object"},
                       {"required", nlohmann::json::array({"name", "why"})},
                       {"additionalProperties", false},
                       {"properties",
                        nlohmann::json{
                            {"name",
                             FieldSchema("Verbatim identifier: a file path, "
                                         "tool name, MCP call, or a topics "
                                         "entry.")},
                            {"why", FieldSchema("Why it is important.")},
                            {"how",
                             FieldSchema("How to use it when continuing. "
                                         "Optional.")}}}}},
                  {"description",
                   "Associative knowledge: links a topic, tool, MCP call, or "
                   "file path to why it is important and how to use it when "
                   "continuing."}}},
             {"lessons",
              StringArraySchema("Solutions to problems actually encountered in "
                                "this session worth keeping so the same work "
                                "is less painful next time; user-defined rules "
                                "and restrictions that apply to this session. "
                                "Not generic best practices -- 'always run "
                                "tests' is filler. Knowledge meant to outlive "
                                "the session belongs in the memory plugin, not "
                                "here.")},
             {"future_tasks",
              StringArraySchema("Work left for after the current task. Free "
                                "text and verbose -- what the task is, why, "
                                "open questions. The complement of the "
                                "checklist tool: the checklist holds the short "
                                "immediate steps of the current task, this "
                                "field holds the longer-horizon work between "
                                "them. File-backed plans (a path a future "
                                "session can read) are strongly advised in "
                                "plan->execute flows.")}}}};
    return schema;
}

std::string SectionHeader(std::string_view title) {
    return "## " + std::string(title) + "\n";
}

void RenderStringList(std::string& out, std::string_view title,
                      const std::vector<std::string>& items) {
    if (items.empty()) return;
    out += SectionHeader(title);
    for (const auto& item : items) out += "- " + item + "\n";
}

std::string OrMarker(const std::string& value) {
    // The degrade path: six good fields must not die because the one required
    // field was omitted, but "nothing" must not render as silence either --
    // the continuing model needs to see that the keystone is missing.
    return value.empty() ? "(current task: not reported)" : value;
}

}  // namespace

std::string RenderStructuredSummary(const StructuredSummary& summary) {
    // Same framing the free-text head uses: one fixed line, then the content.
    // A summary with EVERY field empty still renders the framing and the
    // current-task marker -- the parse layer refuses all-empty before this is
    // called (see compact_messages_structured), so this is defense, not the
    // contract.
    std::string out(kSummaryFraming);
    out += SectionHeader("Current task");
    out += OrMarker(summary.current_task) + "\n";
    RenderStringList(out, "Done", summary.done);
    RenderStringList(out, "Next steps", summary.next_steps);
    RenderStringList(out, "Topics", summary.topics);
    if (!summary.important.empty()) {
        out += SectionHeader("Important");
        for (const auto& item : summary.important) {
            out += "- " + item.name + " -- " + item.why;
            if (!item.how.empty()) out += " (how: " + item.how + ")";
            out += "\n";
        }
    }
    RenderStringList(out, "Lessons", summary.lessons);
    RenderStringList(out, "Future tasks", summary.future_tasks);
    return out;
}

namespace {

// Parse one string field (the required keystone). Absent or non-string
// degrades to empty -- the renderer substitutes the not-reported marker; this
// is the one field whose absence must not nuke the other six.
std::string ParseStringField(const nlohmann::json& input, std::string_view field,
                             const common::Logger& logger, std::string_view where) {
    const auto it = input.find(field);
    if (it == input.end() || it->is_null()) return {};
    if (!it->is_string()) {
        logger.warn("{}summary field '{}' is not a string; ignored", where, field);
        return {};
    }
    return it->get<std::string>();
}

// Parse one string-array field. Non-string items are SKIPPED with a warn, not
// fatal: the summarizer model produced this, not a client, and one bad item
// must not discard the good ones. The logger is the session's (see
// CompactionRequest::logger), so the warn lands beside the compaction's own
// diagnostics.
std::vector<std::string> ParseStringArray(const nlohmann::json& input, std::string_view field,
                                          const common::Logger& logger, std::string_view where) {
    std::vector<std::string> out;
    const auto it = input.find(field);
    if (it == input.end() || it->is_null()) return out;
    if (!it->is_array()) {
        logger.warn("{}summary field '{}' is not an array; ignored", where, field);
        return out;
    }
    out.reserve(it->size());
    for (const auto& item : *it) {
        if (item.is_string()) {
            out.push_back(item.get<std::string>());
        } else {
            logger.warn("{}summary field '{}' has a non-string item; skipped", where, field);
        }
    }
    return out;
}

std::vector<ImportantItem> ParseImportant(const nlohmann::json& input,
                                          const common::Logger& logger,
                                          std::string_view where) {    std::vector<ImportantItem> out;
    const auto it = input.find("important");
    if (it == input.end() || it->is_null()) return out;
    if (!it->is_array()) {
        logger.warn("{}summary field 'important' is not an array; ignored", where);
        return out;
    }
    out.reserve(it->size());
    for (const auto& item : *it) {
        if (!item.is_object()) {
            logger.warn("{}summary field 'important' has a non-object item; skipped", where);
            continue;
        }
        const auto name = item.find("name");
        const auto why = item.find("why");
        // `name` is the field's whole point: an entry without one is unusable,
        // and warning+skip beats rendering "- (no name) -- why".
        if (name == item.end() || !name->is_string() || name->get<std::string>().empty() ||
            why == item.end() || !why->is_string()) {
            logger.warn("{}summary field 'important' has an entry without a string name or "
                        "why; skipped",
                        where);
            continue;
        }
        ImportantItem parsed;
        parsed.name = name->get<std::string>();
        parsed.why = why->get<std::string>();
        const auto how = item.find("how");
        if (how != item.end() && how->is_string()) parsed.how = how->get<std::string>();
        out.push_back(std::move(parsed));
    }
    return out;
}

// Duplicate names kept, not dropped -- the list shape makes them visible (a
// dynamic-key JSON object would have silently collapsed them at parse), and
// two entries sharing a name may still carry different whys worth keeping.
void WarnOnDuplicateNames(const StructuredSummary& summary, const common::Logger& logger,
                          std::string_view where) {
    auto check = [&logger, where](const std::vector<std::string>& items, std::string_view field) {
        auto copy = items;
        std::sort(copy.begin(), copy.end());
        const bool dupes = std::adjacent_find(copy.begin(), copy.end()) != copy.end();
        if (dupes) {
            logger.warn("{}summary field '{}' contains duplicate entries; kept verbatim", where,
                        field);
        }
    };
    check(summary.topics, "topics");
    if (summary.important.size() > 1) {
        std::vector<std::string> names;
        names.reserve(summary.important.size());
        for (const auto& item : summary.important) names.push_back(item.name);
        check(names, "important");
    }
}

std::string_view ToolUseName(const llm::ContentBlock& block) {
    const auto* use = std::get_if<llm::ToolUseBlock>(&block);
    return use == nullptr ? std::string_view{} : std::string_view{use->name};
}

// The shared tail of both branches: the whole-context measurement rule
// compact_messages documents (ground truth when the backend reported one, else
// the bytes/4 estimate over the SAME span -- estimate_tokens is exposed on
// context_compactor.hpp for exactly this reuse).
std::int64_t TokensBefore(const CompactionRequest& params,
                          std::span<const llm::Message> messages_to_compact) {
    return params.last_reported_input_tokens > 0 ? params.last_reported_input_tokens
           : params.full_history                 ? estimate_tokens(*params.full_history)
                                                 : estimate_tokens(messages_to_compact);
}

}  // namespace

std::optional<CompactionResult> compact_messages_structured(
    std::span<const llm::Message> messages_to_compact, llm::LlmProvider& provider,
    const CompactionRequest& params) {
    // MIRRORS compact_messages line for line up to the parse: same logging
    // shape, same refusals, same request fields. The two compacters are kept
    // deliberately parallel -- a fix to one (a new refusal, a routing change)
    // that does not consider the other is a review finding, not an accident.
    const common::Logger& logger = params.logger ? *params.logger : common::DefaultLogger();
    const std::string where = params.session_label.empty()
                                  ? std::string{}
                                  : "session " + std::string(params.session_label) + ": ";

    if (IsBlank(params.compression_prompt)) {
        logger.error("{}compaction skipped: no compression prompt configured", where);
        return std::nullopt;
    }
    llm::ChatRequest request;
    request.system = std::string(params.compression_prompt);
    request.messages = messages_to_compact;
    request.model = std::string(params.model_name);
    request.stream = true;
    request.parameters = params.parameters;
    request.max_tokens = params.max_tokens;
    request.session_id = params.session_id;
    // THE structural difference from compact_messages: the output is
    // constrained by a forced tool call, not asked for in prose. The schema
    // rides the request; the tool is never EXECUTED -- the loop never sees
    // this response, the harness consumes the tool call's input as the
    // summary payload.
    request.tools.push_back(llm::ToolDefinition{std::string(kRecordSummaryTool),
                                                std::string(kRecordSummaryDescription),
                                                RecordSummarySchema()});
    request.tool_choice = llm::ToolChoiceSpec{llm::ToolChoiceKind::Specific,
                                              std::string{kRecordSummaryTool}};

    llm::ChatResponse response;
    try {
        response = provider.StreamComplete(request, [&params](const llm::StreamEvent&) {
            return !params.is_cancelled || !params.is_cancelled();
        });
    } catch (const llm::LlmError& error) {
        logger.warn("{}compaction failed (will retry next turn): {}", where, error.what());
        return std::nullopt;
    }

    if (response.stop_reason == llm::StopReason::Cancelled) return std::nullopt;

    // Before parsing: a summary that hit the output cap is cut off mid-args,
    // and the truncated JSON's failure mode is exactly the unparseable case
    // below -- refusing HERE with the real cause beats a misleading "did not
    // parse" there.
    if (response.stop_reason == llm::StopReason::MaxTokens) {
        logger.error(
            "{}compaction produced a truncated summary (hit max_tokens); skipping -- raise "
            "max_tokens for this provider or shorten the compaction prompt",
            where);
        return std::nullopt;
    }

    const auto call = std::find_if(response.content.begin(), response.content.end(),
                                   [](const llm::ContentBlock& block) {
                                       return ToolUseName(block) == kRecordSummaryTool;
                                   });

    // A provider that ignored tool_choice. If it produced text, degrade to the
    // free-text head rather than refuse: every refusal here re-fires next turn
    // while occupancy climbs toward a hard 400, and a non-conforming backend
    // would otherwise turn EVERY compaction into a failure spiral. Logged so
    // the operator can see the schema is not being honored.
    if (call == response.content.end()) {
        const std::string text = llm::TextOf(response.content);
        if (IsBlank(text)) {
            logger.error("{}compaction produced no tool call and no usable text; skipping",
                         where);
            return std::nullopt;
        }
        logger.warn(
            "{}compaction: the provider ignored tool_choice; using the free-text summary it "
            "returned",
            where);
        CompactionResult degraded;
        degraded.summary_message =
            llm::Message{llm::Role::User, {llm::TextBlock{std::string(kSummaryFraming) + text}}};
        degraded.context_tokens_before = TokensBefore(params, messages_to_compact);
        return degraded;
    }

    const auto& input = std::get<llm::ToolUseBlock>(*call).input;
    if (!input.is_object()) {
        logger.error("{}compaction: the summary tool call's arguments are not a JSON object; "
                     "skipping",
                     where);
        return std::nullopt;
    }

    StructuredSummary summary;
    summary.current_task = ParseStringField(input, "current_task", logger, where);
    summary.done = ParseStringArray(input, "done", logger, where);
    summary.next_steps = ParseStringArray(input, "next_steps", logger, where);
    summary.topics = ParseStringArray(input, "topics", logger, where);
    summary.important = ParseImportant(input, logger, where);
    summary.lessons = ParseStringArray(input, "lessons", logger, where);
    summary.future_tasks = ParseStringArray(input, "future_tasks", logger, where);
    WarnOnDuplicateNames(summary, logger, where);

    // The all-empty case: the call carried the tool name but nothing usable.
    // A keystone-less, content-less summary is not a summary -- refuse rather
    // than fold a header skeleton in place of half the conversation.
    const bool all_empty = summary.current_task.empty() && summary.done.empty() &&
                           summary.next_steps.empty() && summary.topics.empty() &&
                           summary.important.empty() && summary.lessons.empty() &&
                           summary.future_tasks.empty();
    static_assert(sizeof(kFields) / sizeof(kFields[0]) == 7,
                  "kFields must list every StructuredSummary field");
    if (all_empty) {
        logger.error("{}compaction: the summary tool call carried no usable fields; skipping",
                     where);
        return std::nullopt;
    }

    CompactionResult result;
    result.summary_message = llm::Message{llm::Role::User,
                                          {llm::TextBlock{RenderStructuredSummary(summary)}}};
    result.context_tokens_before = TokensBefore(params, messages_to_compact);
    return result;
}

CompactionStrategy MakeStructuredCompactionStrategy() {
    CompactionStrategy strategy;
    // Identical to "summarize"'s cut selection: where to cut is orthogonal to
    // how to summarize, and duplicating the walk here would let the two drift.
    strategy.cut_selector = ProposeSummarizeCut;
    strategy.summarize = compact_messages_structured;
    return strategy;
}

}  // namespace minimal::agent
