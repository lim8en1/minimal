#include "minimal/client/chat_model.hpp"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <ctime>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "minimal/client/utf8.hpp"
#include "minimal/common/logger.hpp"

namespace minimal::client {

std::string FormatClockTime(int64_t unix_ms) {
    if (unix_ms <= 0) return "";
    const std::time_t seconds = static_cast<std::time_t>(unix_ms / 1000);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    char buf[16];
    const int written = std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", local.tm_hour,
                                      local.tm_min, local.tm_sec);
    if (written <= 0) return "";
    return std::string(buf);
}

namespace {

// The result's display payload, or null: absent, malformed and non-object
// payloads all degrade to the text row -- never to a lost row.
nlohmann::json DisplayOf(const v1::ToolResultBlock& block) {
    if (!block.has_display_json()) return nullptr;
    auto display = nlohmann::json::parse(block.display_json(), nullptr, /*allow_exceptions=*/false);
    if (display.is_object()) return display;
    common::DefaultLogger().warn("tool result {}: ignoring malformed display_json",
                                 block.tool_use_id());
    return nullptr;
}

std::string JoinResultText(const v1::ToolResultBlock& block) {
    std::string joined;
    for (const auto& piece : block.content()) {
        if (piece.has_text()) {
            if (!joined.empty()) joined += "\n";
            joined += piece.text().text();
        }
    }
    return joined;
}

// google.protobuf.Struct -> JSON, for the tool_input a renderer renders FROM.
// The server has the same conversion in proto_convert.cpp (ValueToJson); the
// client cannot link server core, and the shape is fixed by protobuf's
// Struct, so this is a twin. ALIGNED with the server's, verbatim semantics:
// faithful recursion into lists (the old ShortDebugString degradation made
// nested list items render as protobuf debug text), and integral doubles
// narrowed back to integers so a `3` sent as 3.0 renders as `3`, not "3.0".
namespace {

bool IsExactInteger(double value) {
    if (!std::isfinite(value)) return false;
    if (value != std::trunc(value)) return false;
    // STRICTLY LESS at the top: static_cast<double>(INT64_MAX) rounds UP to
    // 2^63, and a `<=` against it admits a value whose cast to int64_t is UB.
    constexpr double kTwoToThe63 = 9223372036854775808.0;
    return value >= static_cast<double>(std::numeric_limits<std::int64_t>::min()) &&
           value < kTwoToThe63;
}

nlohmann::json ValueToJson(const google::protobuf::Value& value) {
    switch (value.kind_case()) {
        case google::protobuf::Value::kNullValue: return nullptr;
        case google::protobuf::Value::kNumberValue: {
            const double number = value.number_value();
            if (IsExactInteger(number)) return static_cast<std::int64_t>(number);
            return number;
        }
        case google::protobuf::Value::kStringValue: return value.string_value();
        case google::protobuf::Value::kBoolValue: return value.bool_value();
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
        case google::protobuf::Value::KIND_NOT_SET: break;
    }
    return nullptr;
}

}  // namespace

// The spawn tool's name is core surface (agent_preset.hpp) but the wire is
// what a client sees: a ToolUseBlock named this, whose id a later
// tool-result-only User message echoes. Those messages are the harness
// delivering a subagent's completion -- not tool activity, not the model.
constexpr const char* kSpawnSubagentToolName = "spawn_subagent";

}  // namespace

nlohmann::json StructToJson(const google::protobuf::Struct& value) {
    nlohmann::json out = nlohmann::json::object();
    for (const auto& [key, field] : value.fields()) {
        out[key] = ValueToJson(field);
    }
    return out;
}

// A bounded one-line summary of what the tool was asked to do, for the row
// header: the model's input IS the orientation ("which file", "which
// command"), and a bare "tool bash" line with nothing under it reads as
// noise. Compact JSON, truncated -- the authoritative record is persisted.
std::string InputSummary(const nlohmann::json& input) {
    if (input.is_null() || input.empty()) return "";
    try {
        std::string summary = input.dump();
        if (summary.size() > 120) summary = TruncateUtf8(summary, 120) + "…";
        return summary;
    } catch (const std::exception&) {
        return "";   // a non-serializable input degrades to no summary
    }
}

// bash's own input shape (tools-bash plugin): {"command": "..."}. Every other
// tool falls back to InputSummary's generic JSON dump -- this is a header
// improvement for the single most common tool, not a general input schema.
std::optional<std::string> BashCommandOf(const std::string& tool_name,
                                         const nlohmann::json& input) {
    if (tool_name != "bash") return std::nullopt;
    const auto it = input.find("command");
    if (it == input.end() || !it->is_string()) return std::nullopt;
    return it->get<std::string>();
}

// ask_user's input shape (tool-ask plugin): {"questions": [{"question":
// ..., ...}]}. The question text is the row's orientation ("what was I
// asked") -- the same job bash's $command does for its row -- and the raw
// JSON dump of a full ask request is noise next to it.
std::optional<std::string> AskQuestionOf(const std::string& tool_name,
                                         const nlohmann::json& input) {
    if (tool_name != kAskUserToolName) return std::nullopt;
    const auto questions = input.find("questions");
    if (questions == input.end() || !questions->is_array() || questions->empty() ||
        !(*questions)[0].is_object()) {
        return std::nullopt;
    }
    std::string question = (*questions)[0].value("question", std::string{});
    if (question.empty()) return std::nullopt;
    constexpr std::size_t kMaxQuestion = 120;
    if (question.size() > kMaxQuestion) question = question.substr(0, kMaxQuestion) + "…";
    return question;
}

// The single-line merge of the above, for a caller (the LIVE tool row) that
// has no room for the persisted view's per-kind styling and just wants the
// best available one-liner.
std::string OneLineSummary(std::string summary) {
    if (summary.find('\n') != std::string::npos ||
        summary.find('\r') != std::string::npos) {
        std::string joined;
        joined.reserve(summary.size());
        for (const char c : summary) {
            if (c == '\r') continue;
            if (c == '\n') {
                // A space, not a visible break glyph: an ambiguous-width
                // character here would itself drift the columns this exists
                // to keep aligned.
                if (!joined.empty() && joined.back() != ' ') joined += ' ';
            } else {
                joined += c;
            }
        }
        summary = std::move(joined);
    }
    constexpr std::size_t kMaxLiveSummaryBytes = 200;
    if (summary.size() > kMaxLiveSummaryBytes) {
        summary = TruncateUtf8(summary, kMaxLiveSummaryBytes) + "…";
    }
    return summary;
}

std::string SummarizeToolInput(const std::string& tool_name, const nlohmann::json& input) {
    if (const auto bash_command = BashCommandOf(tool_name, input); bash_command.has_value()) {
        return OneLineSummary("$ " + *bash_command);
    }
    if (const auto ask_question = AskQuestionOf(tool_name, input); ask_question.has_value()) {
        return OneLineSummary(*ask_question);
    }
    return OneLineSummary(InputSummary(input));
}

RowWindowSelection SelectRowWindow(const std::vector<RowSpan>& spans,
                                   int visible_top, int visible_bottom,
                                   int margin_lines,
                                   const std::vector<std::size_t>& force_include) {
    RowWindowSelection selection;
    const long n = static_cast<long>(spans.size());
    if (n == 0) return selection;

    // The intersection test is a linear scan: spans are ascending and the
    // common case is a handful of rows in the margin-widened range, but even
    // the degenerate full pass is O(rows) integer compares -- noise next to
    // the tree walk this seam exists to bound.
    long first = -1;
    long last = -1;
    const int top = std::min(visible_top, visible_bottom) - std::max(0, margin_lines);
    const int bottom = std::max(visible_top, visible_bottom) + std::max(0, margin_lines);
    for (long i = 0; i < n; ++i) {
        if (!spans[i].known()) continue;   // caller full-assembles instead
        if (spans[i].y_max < top || spans[i].y_min > bottom) continue;
        if (first < 0) first = i;
        last = i;
    }
    for (const std::size_t included : force_include) {
        if (static_cast<long>(included) >= n) continue;
        if (!spans[included].known()) continue;   // never laid out: no geometry to extend by
        if (first < 0 || static_cast<long>(included) < first) first = static_cast<long>(included);
        if (static_cast<long>(included) > last) last = static_cast<long>(included);
    }
    if (first < 0) {
        // Nothing intersects (a viewport entirely inside a spacer gap) and
        // nothing is force-included: render the newest row rather than an
        // empty viewport.
        first = n - 1;
        last = n - 1;
    }
    selection.first = static_cast<std::size_t>(first);
    selection.last = static_cast<std::size_t>(last);
    selection.rows_elided_above = first > 0;
    selection.rows_elided_below = last + 1 < n;
    // The elided fillers reproduce the full tree's geometry. Above: the
    // content top through the window's first row (the spans are anchored to
    // the content top, so spans[first].y_min IS that distance). Below: from
    // the window's last row's span end through the LAST row's span end PLUS
    // its trailing spacer -- when rows are elided below, the window's last
    // row skips its own trailing spacer (the filler covers it) and the
    // elided tail's spacer is part of the elided space. When nothing is
    // elided below, the last window row renders its spacer for real (the
    // full path's invariant) and the filler is zero. The live block and
    // pending echoes below that render for real and are not part of the
    // filler.
    selection.above_lines = std::max(0, spans[first].y_min);
    selection.below_lines = 0;
    if (selection.rows_elided_below) {
        selection.below_lines =
            std::max(0, spans.back().y_max + 1 - spans[last].y_max);
    }
    return selection;
}

std::string_view ToString(ChatRowKind kind) {
    switch (kind) {
        case ChatRowKind::UserText: return "user_text";
        case ChatRowKind::UserAttachment: return "user_attachment";
        case ChatRowKind::AgentMarkdown: return "agent_markdown";
        case ChatRowKind::Thinking: return "thinking";
        case ChatRowKind::ToolActivity: return "tool_activity";
        case ChatRowKind::System: return "system";
        case ChatRowKind::Switch: return "switch";
    }
    return "unknown";
}

std::string AssistantLabel(const std::string& agent, const std::string& model) {
    // "openrouter/anthropic/claude-opus-5" -> "claude-opus-5": the full ref is
    // the switch divider's job; a label only needs to be recognizable.
    const auto slash = model.find_last_of('/');
    const std::string short_model = slash == std::string::npos ? model : model.substr(slash + 1);
    if (!agent.empty() && !short_model.empty()) return agent + " (" + short_model + ")";
    if (!agent.empty()) return agent;
    if (!short_model.empty()) return short_model;
    return "assistant";
}

std::vector<ChatRow> ChatRowsFromMessages(const std::vector<v1::StoredMessage>& descending) {
    // Identify the spawn_subagent tool_use ids first: the completion
    // injections are distinguishable ONLY by echoing one of these.
    // unordered_set, not a linear-scanned vector: a scrollback window can
    // hold up to scrollback_window_max_messages (2000 default) messages, and
    // ChatRowsFromMessages runs on every transcript_version bump -- an O(n)
    // membership test per tool-result block made the whole pass O(n^2).
    std::unordered_set<std::string> spawn_ids;
    for (const auto& stored : descending) {
        for (const auto& block : stored.message().content()) {
            if (block.has_tool_use() && block.tool_use().name() == kSpawnSubagentToolName) {
                spawn_ids.insert(block.tool_use().id());
            }
        }
    }
    const auto is_spawn_id = [&](const std::string& id) { return spawn_ids.count(id) != 0; };

    // Walk ascending (reverse of the input), remembering tool names and inputs
    // by id so a result row can carry the name of -- and the input JSON behind
    // -- the call that produced it. unordered_map for the same O(n^2)->O(n)
    // reason as spawn_ids above. Duplicate ids: LAST wins (insert_or_assign) --
    // OpenAI-shaped tool-call fragments are index/id-keyed and the LATER
    // chunks complete the arguments JSON, so the final state is the
    // authoritative one. (An older comment claimed first-wins here; that
    // contradicted the code below and misread the delta semantics.)
    std::vector<ChatRow> rows;
    struct ToolCall {
        std::string name;
        nlohmann::json input = nlohmann::json::object();
    };
    std::unordered_map<std::string, ToolCall> tool_calls;  // id -> call
    const auto tool_call_of = [&](const std::string& id) -> const ToolCall* {
        const auto it = tool_calls.find(id);
        return it != tool_calls.end() ? &it->second : nullptr;
    };

    // Pre-pass over the page for RESULT ids: an assistant tool_use block
    // whose result also sits in this page must not render its own row --
    // that was the "tool bash" with nothing under it, once per call, and
    // the visible reason a transcript read as noise. The result row is the
    // authoritative rendering (it has the output); the tool_use row only
    // matters when the result is NOT here yet (an in-flight call, or a
    // result evicted with an older page -- the window boundary can cut
    // between them), where a pending marker is exactly the right thing.
    std::unordered_set<std::string> result_ids;
    for (const auto& stored : descending) {
        for (const auto& block : stored.message().content()) {
            if (block.has_tool_result()) result_ids.insert(block.tool_result().tool_use_id());
        }
    }

    // The attribution of the last assistant message that HAD one. Unknown
    // (pre-attribution) messages neither set it nor draw a divider: "unknown
    // -> build" is not a switch, it is the history getting a label.
    std::optional<std::pair<std::string, std::string>> last_speaker;

    for (auto it = descending.rbegin(); it != descending.rend(); ++it) {
        const auto& message = it->message();
        const int64_t message_seq = it->seq();
        const std::size_t first_row_of_message = rows.size();

        // A divider BEFORE the first assistant message whose agent or model
        // differs from the previous one's -- /agent, /model and a per-turn
        // override all show up here, where the change actually took effect.
        // Pushed ahead of the message's own rows so the stamp below gives it
        // this message's seq (and the new attribution).
        if (message.role() == v1::ROLE_ASSISTANT &&
            (!it->agent_preset().empty() || !it->model().empty())) {
            std::pair<std::string, std::string> speaker{it->agent_preset(), it->model()};
            if (last_speaker.has_value() && *last_speaker != speaker) {
                ChatRow divider{.kind = ChatRowKind::Switch};
                divider.switch_from_agent = last_speaker->first;
                divider.switch_from_model = last_speaker->second;
                rows.push_back(std::move(divider));
            }
            last_speaker = std::move(speaker);
        }

        // One message's worth of row-pushing, as an IIFE rather than inline
        // loop body: every early exit below used to be a `continue` that
        // skipped the seq-stamping loop that then sat at the bottom of the
        // outer loop, so only the plain-text-user branch (the one case with
        // no early exit) ever got its rows stamped -- every assistant,
        // tool-result and harness-injection row kept seq==-1, the "synthetic"
        // sentinel. ChatView's render cache is keyed on (seq, occurrence),
        // so those rows all collapsed into one bucket keyed by POSITION
        // instead: loading an older page shifts every position, and the
        // cache then serves a stale render under the wrong message. A `return`
        // here ends the lambda, not the outer loop, so the stamp below always
        // runs regardless of which branch produced the rows.
        [&] {
            if (message.role() == v1::ROLE_ASSISTANT) {
                for (const auto& block : message.content()) {
                    if (block.has_text()) {
                        rows.push_back(ChatRow{.kind = ChatRowKind::AgentMarkdown,
                                               .text = block.text().text()});
                    } else if (block.has_thinking()) {
                        rows.push_back(ChatRow{.kind = ChatRowKind::Thinking,
                                               .text = block.thinking().text()});
                    } else if (block.has_tool_use()) {
                        // Computed ONCE, not once for the map entry and again
                        // for the row (the pre-unordered_map shape relied on
                        // vector::back() to share it; insert_or_assign here
                        // keeps that single computation without depending on
                        // "the last pushed entry" being expressible for a map).
                        nlohmann::json input = StructToJson(block.tool_use().input());
                        tool_calls.insert_or_assign(
                            block.tool_use().id(),
                            ToolCall{block.tool_use().name(), input});
                        // No result row in this page for this id: skip the
                        // duplicate -- the result row renders the call WITH
                        // its output. What remains is a pending row only
                        // when the result is genuinely absent (in-flight, or
                        // the window boundary cut between the pair), and the
                        // renderer marks that with the input summary. A plain
                        // `continue` (not the IIFE's `return`): later blocks
                        // of the same message must still produce their rows.
                        if (result_ids.count(block.tool_use().id()) != 0) {
                            continue;
                        }
                        rows.push_back(ChatRow{.kind = ChatRowKind::ToolActivity,
                                               .tool_name = block.tool_use().name(),
                                               .tool_input = std::move(input),
                                               .tool_pending = true});
                    }
                }
                return;
            }

            // ROLE_USER. The harness invariant (openai_request.cpp): a User
            // message is ALL text blocks or ALL tool-result blocks, never
            // mixed.
            bool all_results = !message.content().empty();
            for (const auto& block : message.content()) {
                if (!block.has_tool_result()) all_results = false;
            }

            if (all_results) {
                bool all_spawn = true;
                for (const auto& block : message.content()) {
                    if (!is_spawn_id(block.tool_result().tool_use_id())) all_spawn = false;
                }
                if (all_spawn) {
                    // One harness row per injection, text joined.
                    std::string joined;
                    for (const auto& block : message.content()) {
                        if (!joined.empty()) joined += "\n";
                        joined += JoinResultText(block.tool_result());
                    }
                    rows.push_back(ChatRow{.kind = ChatRowKind::System, .text = std::move(joined)});
                    return;
                }
                for (const auto& block : message.content()) {
                    const ToolCall* call = tool_call_of(block.tool_result().tool_use_id());
                    ChatRow row{.kind = ChatRowKind::ToolActivity,
                                .text = JoinResultText(block.tool_result()),
                                .tool_name = call != nullptr ? call->name : std::string{},
                                .tool_input = call != nullptr ? call->input : nlohmann::json::object(),
                                .tool_display = DisplayOf(block.tool_result()),
                                .tool_is_error = block.tool_result().is_error()};
                    rows.push_back(std::move(row));
                }
                return;
            }

            // Text user message: block 0 is what the human typed (verbatim);
            // later text blocks are @-mention attachments (E3's expansion shape).
            for (int i = 0; i < message.content_size(); ++i) {
                const auto& block = message.content(i);
                if (!block.has_text()) continue;
                if (i == 0) {
                    rows.push_back(
                        ChatRow{.kind = ChatRowKind::UserText, .text = block.text().text()});
                } else {
                    ChatRow row{.kind = ChatRowKind::UserAttachment, .text = block.text().text()};
                    // The E3 frame: <file path="..."> / <file-tree path="...">.
                    const std::string& framed = block.text().text();
                    const auto open = framed.find("<file");
                    const auto path_key = framed.find("path=\"", open);
                    if (path_key != std::string::npos) {
                        const auto end = framed.find('"', path_key + 6);
                        if (end != std::string::npos) {
                            row.attachment_path =
                                framed.substr(path_key + 6, end - (path_key + 6));
                        }
                    }
                    rows.push_back(std::move(row));
                }
            }
        }();

        // Stamp every row this message produced with its transcript seq --
        // the chat view's per-row render cache is keyed by it (track R3) --
        // and the message's wall-clock timestamp, for the chat view's
        // per-row clock display (never a MessageRenderer concern, see
        // ChatRow::created_at_unix_ms). Runs unconditionally now: every
        // branch above returns out of the IIFE rather than `continue`-ing
        // past this.
        for (auto r = rows.begin() + static_cast<std::ptrdiff_t>(first_row_of_message);
             r != rows.end(); ++r) {
            r->seq = message_seq;
            r->created_at_unix_ms = it->created_at_unix_ms();
            if (message.role() == v1::ROLE_ASSISTANT) {
                r->agent = it->agent_preset();
                r->model = it->model();
            }
        }
    }
    return rows;
}

std::vector<bool> LabelContinuations(const std::vector<ChatRow>& rows) {
    std::vector<bool> out(rows.size(), false);
    bool labeled_in_turn = false;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        switch (rows[i].kind) {
            case ChatRowKind::UserText:
            case ChatRowKind::UserAttachment:
            case ChatRowKind::System:
            case ChatRowKind::Switch:
                labeled_in_turn = false;
                break;
            case ChatRowKind::AgentMarkdown:
                out[i] = labeled_in_turn;
                labeled_in_turn = true;
                break;
            case ChatRowKind::Thinking:
            case ChatRowKind::ToolActivity:
                break;
        }
    }
    return out;
}

bool CollapsedByDefault(const ChatRow& row) {
    constexpr std::size_t kAttachmentCollapseThresholdChars = 2000;
    switch (row.kind) {
        case ChatRowKind::Thinking:
            return true;
        case ChatRowKind::ToolActivity:
            // ask_user's result body is the user's recorded choices ("the
            // user selected: ...") -- the only durable confirmation of what
            // they answered, since the modal closes the moment they submit.
            // It renders expanded by default; the collapse toggle still
            // works on it.
            if (row.tool_name == kAskUserToolName) return false;
            // A diff is the review surface for a change the model just made,
            // and its hunks are small by construction: it starts expanded.
            if (row.tool_display.is_object()) {
                const auto kind = row.tool_display.find("kind");
                if (kind != row.tool_display.end() && *kind == "diff") return false;
            }
            return true;
        case ChatRowKind::UserAttachment:
            return row.text.size() > kAttachmentCollapseThresholdChars;
        case ChatRowKind::UserText:
        case ChatRowKind::AgentMarkdown:
        case ChatRowKind::System:
        case ChatRowKind::Switch:
            return false;
    }
    return false;
}

CommandNotice NoticeFromCommandResult(const v1::CommandResult& result) {
    CommandNotice notice;
    notice.message = result.message();
    notice.is_error = result.status() == v1::COMMAND_STATUS_ERROR;
    notice.display = result.display();
    notice.view_hint = result.view_hint();
    return notice;
}

std::optional<int64_t> NextScrollbackCursor(int64_t last_loaded_seq, bool has_earlier) {
    if (last_loaded_seq < 0) return std::nullopt;  // nothing loaded: newest page
    if (!has_earlier) return std::nullopt;         // history exhausted
    return last_loaded_seq;
}

}  // namespace minimal::client
