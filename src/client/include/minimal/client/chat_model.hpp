#pragma once

// The data plane of the chat view, split from FTXUI so it is testable
// (ARCHITECTURE.md "Testing": FTXUI render output is not unit-tested -- the
// decisions behind it are).
//
// Message-kind rendering (ARCHITECTURE.md "Message-kind rendering") maps the
// persisted transcript to display rows; the extension seam for PLUGGABLE
// presentation of these kinds is E5 (docs/EXTENSIBILITY.md §4.2) and slots in
// exactly here -- the data plane stays core regardless of who renders a row.
//
// COMMAND RESULTS NEVER ENTER THE CHAT (docs/EXTENSIBILITY.md §4.4): they
// become transient notices, a type disjoint from ChatRow. E4 renders every
// CommandResult the same way (the CommandDisplay refinement is E5); the
// never-a-chat-row half of the ruling holds now, enforced by the type split
// below rather than by a check someone could forget.

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "minimal/v1/session_service.pb.h"

namespace minimal::client {

enum class ChatRowKind {
    UserText,        // verbatim styled text -- echoes typed input, NOT markdown
    UserAttachment,  // an @-mention expansion block (E3): path-framed content
    AgentMarkdown,   // markdown-rendered agent text
    Thinking,        // collapsible, dimmed, collapsed by default
    ToolActivity,    // collapsible one-liner; expandable body where one exists
    System,          // harness: subagent-completion injections (real messages,
                     // because the model must see them at the turn boundary)
    Switch,          // synthetic divider: the agent and/or model changed
                     // between two assistant messages (agent/model = new,
                     // switch_from_* = old). Never collapsible.
};

// Stable diagnostic name -- registry collision reports and startup warnings
// name kinds by it.
std::string_view ToString(ChatRowKind kind);

struct ChatRow {
    // The transcript row this view row came from (first member: designated
    // initializers elsewhere stay declaration-ordered). -1 = synthetic.
    int64_t seq = -1;
    ChatRowKind kind;
    // UserText/AgentMarkdown/Thinking: the text itself. UserAttachment: the
    // framed block (the view extracts the path for the header). ToolActivity:
    // the expandable body, empty for a bare tool_use. System: the injected
    // result text.
    std::string text;
    std::string tool_name;        // ToolActivity only
    // ToolActivity only: the input JSON the model sent with the call, so a
    // tool-result renderer can show what was rendered FROM (a diff renderer
    // needs the file path, not just the hunk text). Empty object when unknown.
    nlohmann::json tool_input = nlohmann::json::object();
    // ToolActivity result rows only: the tool's display payload
    // ({"kind": ..., ...}), null when it sent none or sent a malformed one.
    nlohmann::json tool_display = nullptr;
    std::string attachment_path;  // UserAttachment: the path="..." from the frame
    bool tool_is_error = false;   // ToolActivity only
    // ToolActivity only: true for a tool_use whose tool_result is NOT in the
    // same scrollback page (an in-flight call, or a window boundary cut
    // between the pair) -- the renderer shows the input summary instead of a
    // body. A result-bearing row never sets it.
    bool tool_pending = false;
    // The originating message's StoredMessage.created_at_unix_ms; 0 for a
    // synthetic row (seq == -1), which has no backing message to date it.
    // Rendered by ChatView itself (layout, not per-kind presentation), never
    // by a MessageRenderer -- see chat_view.cpp.
    int64_t created_at_unix_ms = 0;
    // Rows from an ASSISTANT message: the agent preset and "<provider>/<model>"
    // that produced it (StoredMessage.agent_preset/model). Empty = unknown --
    // user/tool-result rows, and anything persisted before attribution.
    std::string agent;
    std::string model;
    // Switch only: the attribution of the previous assistant message.
    std::string switch_from_agent;
    std::string switch_from_model;
};

// The speaker label for an assistant row: "build (sonnet-4-5)" -- the agent
// preset with the model's last path segment. Degrades to whichever half is
// known, and to "assistant" when neither is.
std::string AssistantLabel(const std::string& agent, const std::string& model);

// Formats a wall-clock timestamp for a chat row: local "HH:MM:SS", or empty
// for 0 (no timestamp known -- a synthetic row). Local time, not UTC: this is
// what a human reads next to a message they just watched arrive.
std::string FormatClockTime(int64_t unix_ms);

// The kind's own built-in collapse default -- the least-specific tier of the
// per-row/global/kind-default resolution (ChatView owns the other two
// tiers). Shared between ChatView (which resolves the state) and core's
// renderers (which render collapsed vs. expanded from it), so the threshold
// cannot drift between "starts collapsed" and "looks collapsed". Thinking is
// always collapsed by default; ToolActivity collapses unless the row is an
// ask_user result -- its body IS the user's recorded choices, their only
// durable confirmation of what they answered (the modal closes the moment
// they submit); UserAttachment collapses only once its content passes a
// small size threshold -- most tool calls and most mentions are short
// enough to just read.
bool CollapsedByDefault(const ChatRow& row);

// The tool-ask plugin's tool name, as the transcript's tool_use blocks carry
// it. Shared by the collapse default and the row renderer so the two
// special cases cannot drift apart (the plugin lives server-side; the client
// has only the literal).
inline constexpr std::string_view kAskUserToolName = "ask_user";

// Rows in DISPLAY order (oldest first) from a DESCENDING-seq scrollback page --
// exactly what GetMessages returns, so no caller re-sorts. User messages that
// are tool-result-only are tool activity, EXCEPT the ones keyed to a
// spawn_subagent call: those are the harness speaking, and render as System.
std::vector<ChatRow> ChatRowsFromMessages(const std::vector<v1::StoredMessage>& descending);

// Parallel to `rows`: true for an AgentMarkdown row that should NOT repeat the
// speaker label, because an earlier assistant text row of the same turn
// already carried it. A turn boundary -- a user row, a harness (System) row or
// a Switch divider -- resets it; thinking and tool rows belong to the turn and
// neither label nor reset. Every other kind is false.
std::vector<bool> LabelContinuations(const std::vector<ChatRow>& rows);

// google.protobuf.Struct -> JSON, for a tool's input/args. Exposed here (the
// definition lives in chat_model.cpp) so callers outside the transcript path
// -- e.g. SessionState::OnToolStarted, which needs the SAME conversion for
// the LIVE tool_use_id it just opened, not a persisted StoredMessage -- can
// reuse it instead of re-deriving it from google::protobuf::Value by hand.
nlohmann::json StructToJson(const google::protobuf::Struct& value);

// A bounded one-line dump of a tool's raw input JSON (truncated at 120
// chars), for a tool with no dedicated special case below.
std::string InputSummary(const nlohmann::json& input);

// bash's own $command, if `tool_name` is "bash" and `input` has one.
std::optional<std::string> BashCommandOf(const std::string& tool_name, const nlohmann::json& input);

// ask_user's own question text, if `tool_name` is kAskUserToolName and
// `input` has one.
std::optional<std::string> AskQuestionOf(const std::string& tool_name, const nlohmann::json& input);

// The single-line merge of the three above -- bash's $command, ask_user's
// question, or the generic JSON dump, in that preference order. Shared by
// the persisted ToolActivity renderer (core_registrations.cpp, which also
// uses the three individually for their distinct per-kind styling) and the
// LIVE tool row renderers (ChatView::RenderLiveToolRow, RenderToolsPanel),
// which have room for only one dim line and want it to read the same
// whether the call is still running or already in the transcript.
std::string SummarizeToolInput(const std::string& tool_name, const nlohmann::json& input);

// CommandResult -> transient notice line. The return type is deliberately not
// a ChatRow and not convertible to one: a command result has no path into the
// chat model at compile time, which is the invariant (§4.4) expressed where it
// cannot regress. Display carries the CommandDisplay refinement (E5): Status
// renders on the notice line, View opens the view named by view_hint, Silent
// is status-bar-only -- and an ERROR always degrades to the status line, the
// dispatcher having already downgraded it server-side.
struct CommandNotice {
    std::string message;
    bool is_error = false;
    v1::CommandDisplay display = v1::COMMAND_DISPLAY_STATUS;
    std::string view_hint;
};
CommandNotice NoticeFromCommandResult(const v1::CommandResult& result);

// The keyset cursor for the NEXT scrollback page.
//   Nothing loaded yet      -> nullopt: fetch the newest page (no cursor).
//   has_earlier             -> the oldest seq loaded (exclusive upper bound).
//   history exhausted       -> nullopt, and the view stops asking.
// One function for both "should I fetch" and "with what cursor" so the two
// cannot drift -- a view that computed its own cursor would be a second
// opinion about the keyset contract.
std::optional<int64_t> NextScrollbackCursor(int64_t last_loaded_seq, bool has_earlier);

// The frame pass's newest-slice refresh decision (the state machine over
// SessionState::Snapshot) lives in scrollback_refresh.hpp -- it needs
// SessionState's definition, which includes THIS header, so it cannot live
// here without a cycle.

}  // namespace minimal::client
