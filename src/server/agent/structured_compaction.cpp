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

// The tool call whose input carries the structured summary. Declared on EVERY
// main-loop request (see MakeRecordSummaryToolDefinition) so the summarizer
// request -- the session's own prefix plus one instruction -- keeps the tools
// array byte-identical and reads the history from the prompt cache. Because it
// rides every request, the definition is kept to names and types: the
// guidance for each field lives in prompts/compression-structured.md, which
// is only sent in the compaction-time instruction.
constexpr std::string_view kRecordSummaryDescription =
    "Record the context summary. Only call this when the harness asks for one.";

// One source of truth for the wire property order and the render order; the
// parser walks the declared fields of StructuredSummary directly, so this is
// documentation, not a lookup table.
constexpr std::array<std::string_view, 7> kFields = {
    "current_task", "done", "next_steps", "topics", "important", "lessons", "future_tasks"};

nlohmann::json StringField() { return nlohmann::json{{"type", "string"}}; }

nlohmann::json StringArray() {
    return nlohmann::json{{"type", "array"}, {"items", StringField()}};
}

// Built once. Strict-mode shape (the definition is sent with strict: true):
// every property required, additionalProperties false at every level, no
// minItems. "Nothing to say" is an empty array or string, which the parse
// treats as "section absent". Backends that do not enforce strict still get
// the schema as a hint, and the parse degrades rather than refusing.
const nlohmann::json& RecordSummarySchema() {
    static const nlohmann::json schema{
        {"type", "object"},
        {"required", nlohmann::json::array({"current_task", "done", "next_steps", "topics",
                                            "important", "lessons", "future_tasks"})},
        {"additionalProperties", false},
        {"properties",
         nlohmann::json{
             {"current_task", StringField()},
             {"done", StringArray()},
             {"next_steps", StringArray()},
             {"topics", StringArray()},
             {"important",
              nlohmann::json{
                  {"type", "array"},
                  {"items",
                   nlohmann::json{
                       {"type", "object"},
                       {"required", nlohmann::json::array({"name", "why", "how"})},
                       {"additionalProperties", false},
                       {"properties", nlohmann::json{{"name", StringField()},
                                                     {"why", StringField()},
                                                     {"how", StringField()}}}}}}},
             {"lessons", StringArray()},
             {"future_tasks", StringArray()}}}};
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

llm::ToolDefinition MakeRecordSummaryToolDefinition() {
    llm::ToolDefinition definition{std::string(kRecordSummaryToolName),
                                   std::string(kRecordSummaryDescription), RecordSummarySchema()};
    definition.strict = true;
    return definition;
}

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

    // A custom prompt (preset frontmatter, /compact <name>) describes its own
    // output shape; the schema would override it. Free text honors it. So does
    // a missing prompts/compression-structured.md (the sanity check says so).
    if (params.custom_prompt || IsBlank(params.structured_prompt)) {
        return compact_messages(messages_to_compact, provider, params);
    }

    // THE structural difference from compact_messages: the output is a
    // record_summary call, not prose. Not FORCED -- a forced tool_choice
    // invalidates the messages cache and is a 400 on newer Claude models --
    // but strict, so when the model calls it the arguments match the schema.
    // The tool is never EXECUTED: the loop never sees this response, the
    // harness consumes the call's input as the summary payload.
    std::vector<llm::Message> storage;
    llm::ChatRequest request = BuildSummarizerRequest(
        messages_to_compact, params, std::string(params.structured_prompt), storage);
    // The loop's own tools already carry it; a direct caller's may not.
    const bool has_tool = std::any_of(request.tools.begin(), request.tools.end(),
                                      [](const llm::ToolDefinition& def) {
                                          return def.name == kRecordSummaryToolName;
                                      });
    if (!has_tool) {
        request.tools.push_back(MakeRecordSummaryToolDefinition());
        std::sort(request.tools.begin(), request.tools.end(),
                  [](const llm::ToolDefinition& a, const llm::ToolDefinition& b) {
                      return a.name < b.name;
                  });
    }

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
                                       return ToolUseName(block) == kRecordSummaryToolName;
                                   });

    // The model answered without calling the tool (tool_choice is auto, and
    // some backends do not do tools well). If it produced text, degrade to the
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
            "{}compaction: the model did not call {}; using the free-text summary it returned",
            where, kRecordSummaryToolName);
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
