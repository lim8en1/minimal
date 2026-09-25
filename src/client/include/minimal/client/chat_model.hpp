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

#include <chrono>
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
    Compaction,      // the server compacted the working context after this
                     // row's seq: collapsed to a one-line header, expands to
                     // the summary (markdown) the model now sees.
};

// Stable diagnostic name -- registry collision reports and startup warnings
// name kinds by it.
std::string_view ToString(ChatRowKind kind);

inline constexpr int kSwitchSlot = -1;
inline constexpr int kCompactionSlotBase = 1 << 20;

struct ChatRow {
    // The transcript row this view row came from (first member: designated
    // initializers elsewhere stay declaration-ordered). -1 = synthetic.
    int64_t seq = -1;
    // Which part of message `seq` this row renders -- a CONTENT identity,
    // stable however the rest of the page changes: the source content
    // block's index, kSwitchSlot for the divider ahead of the message, or
    // kCompactionSlotBase + k for the k-th compaction after it. (seq, slot)
    // is the chat view's per-row cache and geometry key. The old key --
    // the row's occurrence index within its seq -- shifted whenever a
    // pending tool_use row dropped out (its result arrived) or a divider
    // came or went, and the cache then served one row's render for another.
    int slot = 0;
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
    // Compaction only: what the fold did. `text` holds the summary.
    int64_t compaction_folded_messages = 0;
    int64_t compaction_tokens_before = 0;
    int64_t compaction_tokens_after = 0;
    std::string compaction_trigger;   // "auto" | "manual"
    // A sanity guard's message, shown even collapsed; skipped = nothing was
    // folded (the row is a notice, `text` is empty).
    std::string compaction_warning;
    bool compaction_skipped = false;
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
// always collapsed by default; ToolActivity collapses unless the row carries a
// diff (a display renderer can also claim a default for its own kind -- see
// DisplayRenderer::collapsed_by_default, which ChatView consults first);
// UserAttachment collapses only once its content passes a
// small size threshold -- most tool calls and most mentions are short
// enough to just read.
bool CollapsedByDefault(const ChatRow& row);

// Rows in DISPLAY order (oldest first) from a DESCENDING-seq scrollback page --
// exactly what GetMessages returns, so no caller re-sorts. User messages that
// are tool-result-only are tool activity, EXCEPT the ones keyed to a
// spawn_subagent call: those are the harness speaking, and render as System.
//
// `compactions` (ascending by at_seq, may be null) interleave as Compaction
// rows right after the rows of the message whose seq is their at_seq; one
// whose message is not in the page is skipped (it belongs to another page).
std::vector<ChatRow> ChatRowsFromMessages(
    const std::vector<v1::StoredMessage>& descending,
    const std::vector<v1::CompactionEvent>* compactions = nullptr);

// The compaction row's one-line header: "context compacted (auto): 412
// messages summarized, ~712k -> ~168k tokens", or "compaction skipped (auto)"
// for a guard's notice (the warning renders on its own line). Shared by the
// core renderer and tests.
std::string CompactionHeadline(const ChatRow& row);

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

// Collapses a tool summary to the single line the LIVE tool row and the
// running-tools panel render it as. text() declares itself ONE row tall
// regardless of embedded '\n's (the bug class frame_renderer.cpp's notice
// path documents), so a multi-line bash command -- a heredoc, a `cd x && \`
// continuation, a subagent's tool call -- spilled over every row below it.
// Newlines collapse to spaces; overlong commands truncate.
std::string OneLineSummary(std::string summary);

// The single-line merge of the two above -- bash's $command or the generic
// JSON dump, in that preference order. Shared by the persisted ToolActivity
// renderer (core_registrations.cpp, which also uses the two individually for
// their distinct per-kind styling) and the
// LIVE tool row renderers (ChatView::RenderLiveToolRow, RenderToolsPanel),
// which have room for only one dim line and want it to read the same
// whether the call is still running or already in the transcript.
std::string SummarizeToolInput(const std::string& tool_name, const nlohmann::json& input);

// ============================================================================
// Scrollback row windowing (ChatView::Render). The chat column's Element
// tree contains every row of the scrollback window, and FTXUI walks the
// whole tree every frame -- requirement (word wrap), SetBox, and cell
// composition are each O(transcript). Windowing builds only the rows whose
// content-coordinate span intersects the viewport (plus force-includes),
// representing the elided space with fixed-height fillers, so the per-frame
// walk is O(viewport). Content height is preserved by the fillers, so the
// scroll offset math (chat_scroll_region.hpp) is unchanged.
//
// The seam is the pure part: SELECTING the window from per-row geometry.
// Measuring the geometry lives in ChatView (RowBoxSpy boxes -> content
// coordinates, one frame stale by construction -- the same convergence
// every ScrollState read already accepts). Spans are COMPLETE when the
// selector runs: ChatView full-assembles (and measures) whenever any row
// has never been laid out, so no estimation enters the math.
// ============================================================================

// One rendered row's vertical extent in CONTENT coordinates (lines,
// inclusive [y_min, y_max]), as laid out by the last pass that included the
// row, trailing spacer line included. Content coordinates are anchored to
// the top of the scroll region's child box, so they are stable across
// scroll offsets and content growth below -- the invariant that lets an
// off-window row's last-measured span stay valid.
struct RowSpan {
    int y_min = 0;
    int y_max = -1;   // < y_min marks "never laid out"
    bool known() const { return y_max >= y_min; }
    int lines() const { return y_max - y_min + 1; }
};

struct RowWindowSelection {
    std::size_t first = 0;   // inclusive row index range of the window
    std::size_t last = 0;
    bool rows_elided_above = false;   // first > 0: the transcript continues above
    bool rows_elided_below = false;   // last + 1 < row count
    // Elided line counts. above_lines spans everything above the window's
    // first row -- top filler slack, boundary markers, elided rows and their
    // spacers (the caller renders none of those when rows are elided above).
    // below_lines spans elided rows and their spacers down to the LAST row's
    // span end; the region below that (live streaming block, pending echoes)
    // always renders for real below the filler, so it must not be counted.
    // The window's last row does not render its own trailing spacer when
    // rows are elided below (the filler covers it). Both clamped >= 0; the
    // fillers keep the column's total content height -- and therefore every
    // scroll offset -- what the full tree would have measured.
    int above_lines = 0;
    int below_lines = 0;
};

// Rows whose span intersects [visible_top, visible_bottom] (content
// coordinates, inclusive), widened by `margin_lines` on both sides to absorb
// the one-frame staleness of the spans (content that grew or scrolled since
// the spans were measured), unioned with `force_include` indices (the row
// cursor's target, a just-toggled row), then closed over the contiguous
// index range [first, last]. Everything outside the range is elided into
// above_lines/below_lines. `spans` must be complete (every span known) and
// ascending with the rendered row order. A selection that would be empty
// (visible range in a gap between rows, no force-includes) falls back to
// the LAST row so the viewport never renders nothing.
RowWindowSelection SelectRowWindow(const std::vector<RowSpan>& spans,
                                   int visible_top, int visible_bottom,
                                   int margin_lines,
                                   const std::vector<std::size_t>& force_include);

// How many focused-tab UI-version bumps (SessionState::TickNotice, driven by
// TabMaintenance::Run) a notice survives before it clears itself. Not a
// wall-clock TTL: ticking is gated on the SAME UiVersion check that gates a
// repaint, so a notice on an otherwise-idle screen stays put (there is
// nothing to wake the tick), and fades once the user resumes actually doing
// something -- which is when a stale "/theme x" or "/cost" line stops being
// useful anyway. Picked by feel; tune if it reads as too eager or too sticky.
inline constexpr int kNoticeTicksDefault = 20;
// ...and never before it has been on screen this long. The ticks count
// app-wide UI activity, and a background tab streaming deltas is activity
// too: 20 ticks alone could clear a notice in well under a second, before
// anyone read it.
inline constexpr std::chrono::milliseconds kNoticeMinVisible{4000};

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
    // Counts down in SessionState::TickNotice; the notice clears itself at
    // zero. Every construction site (NoticeFromCommandResult, OnError's and
    // OnSendFailed's and NoteRefreshFailed's aggregate-inits) leaves this at
    // its default -- there is exactly one lifetime policy, not one per call
    // site.
    int ticks_remaining = kNoticeTicksDefault;
    // When the notice was raised -- the kNoticeMinVisible floor.
    std::chrono::steady_clock::time_point shown_at = std::chrono::steady_clock::now();
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
