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
};

struct ChatRow {
    ChatRowKind kind;
    // UserText/AgentMarkdown/Thinking: the text itself. UserAttachment: the
    // framed block (the view extracts the path for the header). ToolActivity:
    // the expandable body, empty for a bare tool_use. System: the injected
    // result text.
    std::string text;
    std::string tool_name;       // ToolActivity only
    std::string attachment_path; // UserAttachment: the path="..." from the frame
    bool tool_is_error = false;  // ToolActivity only
};

// Rows in DISPLAY order (oldest first) from a DESCENDING-seq scrollback page --
// exactly what GetMessages returns, so no caller re-sorts. User messages that
// are tool-result-only are tool activity, EXCEPT the ones keyed to a
// spawn_subagent call: those are the harness speaking, and render as System.
std::vector<ChatRow> ChatRowsFromMessages(const std::vector<v1::StoredMessage>& descending);

// CommandResult -> transient notice line. The return type is deliberately not
// a ChatRow and not convertible to one: a command result has no path into the
// chat model at compile time, which is the invariant (§4.4) expressed where it
// cannot regress.
struct CommandNotice {
    std::string message;
    bool is_error = false;
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

}  // namespace minimal::client
