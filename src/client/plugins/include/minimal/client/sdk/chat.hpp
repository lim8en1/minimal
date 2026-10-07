#pragma once

// The chat surface slot (EXCLUSIVE, REQUIRED): one pane per tab, fed each
// frame from core's data plane, drawing rows through the renderer
// registries its host hands out.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include "minimal/client/sdk/prompt_editor.hpp"
#include "minimal/client/sdk/registries.hpp"
#include "minimal/client/sdk/theme.hpp"
#include "minimal/v1/session_service.pb.h"

namespace minimal::client::plugins {

// --- Chat surface (EXCLUSIVE, REQUIRED) -------------------------------------

// A message queued behind the running turn, until the server takes it.
struct ChatPendingMessage {
    std::uint64_t turn_id = 0;
    std::string text;
};

// One piece of the turn in flight, in arrival order: assistant text, a
// thinking block, or a tool call.
struct ChatLiveSegment {
    enum class Kind { Text, Tool, Thinking };
    Kind kind = Kind::Text;
    // Stable for the segment's life and never reused: key per-segment state
    // (a markdown stream) on it.
    std::uint64_t segment_id = 0;
    // Text and Thinking: everything so far, shared and immutable. Never null.
    std::shared_ptr<const std::string> text = std::make_shared<const std::string>();
    bool text_truncated = false;  // the live text was capped; the transcript has it all
    // Tool: the call -- live, or its final state once the turn ended and the
    // transcript has not caught up yet. Unset: nothing to draw yet.
    std::optional<ToolActivity> tool;
};

// One tab's chat as it stands, built by core each frame from its data plane:
// the transcript window (raw, newest first -- ChatRowsFromMessages
// materializes rows), the turn in flight, the queue. UI thread.
struct ChatFrame {
    std::string session_id;
    // Moves whenever the transcript window changed (per session: key caches
    // on session_id too). rewind_epoch moves when a rewind cut it.
    std::uint64_t transcript_version = 0;
    std::uint64_t rewind_epoch = 0;
    std::shared_ptr<const std::vector<v1::StoredMessage>> scrollback_page;
    std::shared_ptr<const std::vector<v1::CompactionEvent>> compactions;
    bool has_earlier_messages = false;  // older pages exist (chat.load_older)
    bool scrollback_trimmed = false;    // the window evicted its newest pages
    bool scrollback_gap = false;        // a hole between the window and the live turn
    // Effective (what the session runs) and desired (staged, unconfirmed).
    std::string model, agent_preset, desired_model, desired_agent_preset;
    std::vector<ChatPendingMessage> pending_user_messages;
    std::vector<ChatLiveSegment> live_segments;
    // The latest text segment's block just ended: its markdown must be
    // reparsed authoritatively (MarkdownEngine::Stream::Finalize).
    bool streaming_finalized = false;
    bool turn_busy = false;   // thinking, working or waiting
};

// What core gives a chat pane. UI thread.
class ChatHost {
public:
    virtual ~ChatHost() = default;
    // The row renderers (claims and the bundled fallbacks) and the engine.
    virtual const MessageRendererRegistry& MessageRenderers() const = 0;
    virtual const ToolResultRendererRegistry& ToolRenderers() const = 0;
    virtual const DisplayRendererRegistry& DisplayRenderers() const = 0;
    virtual const MarkdownEngine& Markdown() const = 0;
    // The key `action` is bound to ("" when none), for hints in the text.
    virtual std::string KeyFor(std::string_view action) const = 0;
    // Cancels a queued message of tab `tab_id` before it runs.
    virtual void CancelQueued(const std::string& tab_id, std::uint64_t turn_id) = 0;
    // A transient line on tab `tab_id` (feedback for an action: "thinking
    // rows: collapsed (saved)").
    virtual void Notice(const std::string& tab_id, const std::string& text) = 0;
    virtual void RequestRedraw() = 0;
};

// One tab's chat body. Core shows it in the tab's body row and feeds it.
class ChatPane {
public:
    virtual ~ChatPane() = default;
    virtual ftxui::Element Render(const ChatFrame& frame) = 0;
    // One of the surface's declared actions (ChatSurface::Actions), pressed
    // with this tab focused.
    virtual void OnAction(std::string_view action) = 0;
    // A mouse event (wheel, click) with this tab focused. True = consumed
    // (it was over the chat and meant something there).
    virtual bool OnMouse(const ftxui::Event& event) = 0;
    // The user just sent a message on this tab: follow the newest.
    virtual void OnSubmitted() = 0;
};

// The chat view: one pane per tab, made on the tab's first frame. Required:
// the TUI shows nothing without it. A pane that throws shows an error line
// for that tab instead.
class ChatSurface {
public:
    virtual ~ChatSurface() = default;
    virtual std::vector<DeclaredAction> Actions() const = 0;
    // `host` and `theme` outlive the pane.
    virtual std::unique_ptr<ChatPane> Make(ChatHost& host, const std::string& tab_id,
                                           const Theme& theme) = 0;
};

}  // namespace minimal::client::plugins
