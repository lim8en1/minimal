#pragma once

// Per-tab UI caches and the frame's maintenance pass, factored out of
// RunTui (app.cpp). TabUi is genuinely UI-plumbing data (a streaming view,
// grant-checkbox state, a ChatView instance) that RunTui's still-inline
// render body indexes directly by session id -- it stays a plain
// std::map<std::string, TabUi> RunTui owns, the same as before; only the
// LOGIC that maintains it (TabMaintenance) moves into a class here.
//
// TabMaintenance does NOT own `tab_ui`: it takes it by reference in
// Dependencies, the same pattern KeyContext and PickerController::
// Dependencies already use for everything else RunTui owns. This matters
// for declaration ORDER, not just style -- close_tab (RunTui, defined
// before TabMaintenance can be constructed: it needs open_picker/
// plugin_modal_child, which are built later) erases from `tab_ui` directly,
// so the map itself must exist as an ordinary RunTui local from the point
// TabUi's type is known, not appear only once every dependency of the
// maintenance logic is available.

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include <ftxui/component/component.hpp>

#include "minimal/client/plugins/registries.hpp"
#include "minimal/client/render/streaming_markdown_view.hpp"
#include "minimal/client/render/theme.hpp"
#include "minimal/client/scrollback_refresh.hpp"
#include "minimal/client/state.hpp"
#include "minimal/client/view/chat_view.hpp"
#include "minimal/client/view/prompt_modal.hpp"

namespace minimal::client {

// Whether a SHARED modal slot (see TabMaintenance::MaintainModals) must be
// rebuilt: either this is genuinely a new request for the tab whose id is
// tracked alongside the old one, or focus has moved to a different tab
// since the slot was last filled. The rebuild-on-new-request-id bookkeeping
// is per-tab (TabUi), but permission_child/confirmation_child/ask_child are
// each ONE slot shared by every tab -- so a tab switch alone (A -> B -> A,
// A's own request id unchanged throughout) can leave the slot holding B's
// (possibly already-answered) modal while A's guard reads "unchanged,
// nothing to rebuild." Pure so the rebuild decision is directly testable
// without TabMaintenance's AppState/FTXUI dependencies.
bool ModalSlotStale(const std::string& tracked_request_id, const std::string& request_id,
                    const std::string& slot_tab, const std::string& focused_tab);

struct TabUi {
    // Track C4: one slot per SessionState::Snapshot::live_segments entry,
    // index-aligned (ChatView::LiveTextStreamSlot -- a Tool-kind index's
    // slot stays default/unused). Replaces the single shared `streaming`
    // view a pre-C4 tab held: text before and after a tool call are now
    // SEPARATE segments, each needing its own debounce/finalize lifecycle,
    // not one buffer the tool row rendered unconditionally after in full.
    std::vector<LiveTextStreamSlot> live_text_streams;
    // The permission modal's toggles are owned by the modal/closure pair
    // (a shared_ptr captured by both, see TabMaintenance::MaintainModals) --
    // there is deliberately no TabUi member holding them: a raw unique_ptr
    // here once made the closure's validity depend on an implicit
    // "never reset while its modal lives" invariant.
    std::string grants_for_request;
    // The wire confirmation modal's rebuild key, like grants_for_request:
    // the inner modal is rebuilt only when a NEW request_id arrives.
    std::string confirmation_for_request;
    // The model's ask modal (tool-ask plugin), same rebuild keying.
    std::string ask_for_request;
    ftxui::Component surface_body;          // the chat surface's tab body (when slotted)
    plugins::ChatSurface* surface_for = nullptr;   // which surface instance built it
    // Per-tab ChatView: its row cache is keyed by transcript_version, a
    // PER-SESSION counter starting at 0 -- one shared instance served tab
    // A's cached rows as tab B's transcript whenever the two versions
    // collided. One view per tab makes the invariant true by construction.
    std::unique_ptr<ChatView> chat;
    // VIEW-display notice side-effects fire ONCE per notice, keyed on the
    // notice's sequence number (see TabMaintenance::HandleNotice).
    uint64_t handled_notice_seq = 0;
    // Streaming bookkeeping (see TabMaintenance::Maintain): turn_seq keys
    // recreation of the WHOLE live_text_streams vector (a new turn starts a
    // fresh timeline); each slot's own finalized_for_turn fires that
    // segment's one authoritative reparse.
    uint64_t streaming_turn = 0;
    // The newest-slice refresh state machine (T2, reworked): submitted on a
    // version mismatch, resolved via Snapshot::refresh_seq, with the
    // submit/latch/mark-current decision in AdvanceScrollbackRefresh
    // (chat_model.hpp) -- the pure seam where the "user prompt seen only on
    // the first message" bug is now unit-tested.
    ScrollbackRefreshState refresh;
};

// THE MAINTENANCE PASS: everything the frame needs but must not do from
// Render -- a fresh focused-tab snapshot, the per-tab streaming/refresh
// bookkeeping, and the once-per-notice VIEW side-effects (a View-display
// CommandResult may stage a modal and run a bounded RPC; doing that inside
// the renderer made the frame stall and the staged modal unclosable). Run()
// is meant to be called from the event path (RunTui's root CatchEvent),
// never from a Renderer.
class TabMaintenance {
public:
    struct Dependencies {
        AppState& app;
        plugins::MarkdownEngine& engine;
        // No by-value ChatSurface* here: Maintain() gates SOLELY on
        // AppState::EffectiveChatSurface(), which is what actually renders
        // the tab body (FrameRenderer::Render reads the same accessor). A
        // constant-since-bootstrap copy used to be checked ALONGSIDE it,
        // which made Maintain() skip its own bookkeeping forever once a
        // surface was ever registered -- including after
        // AppState::DisableChatSurface flipped the effective accessor back
        // to null and the frame fell back to the core ChatView, which then
        // never got fed a streaming delta or a scrollback refresh.
        plugins::ViewRegistry& view_registry;
        std::vector<plugins::PluginViewEntry>& plugin_views;
        std::shared_ptr<ftxui::Component>& plugin_modal_child;
        bool& show_plugin_modal;
        std::function<void()> open_picker;
        std::function<void()> open_agent_picker;
        std::function<void()> open_model_picker;
        std::function<void()> open_subagent_model_picker;
        std::function<void()> open_mcp_picker;
        // Drains PickerController's async fetch completions and rebuilds
        // whichever picker's component they belong to (PickerController::Poll).
        // Called every pass BEFORE the version-gate below can skip it: a
        // completion's own NotifyExternalUpdate() is what bumps UiVersion()
        // and gets Run() past that gate in the first place, so placing this
        // call anywhere else would either miss it or need its own separate
        // wake path. A picker fetch that runs on AppState's RPC worker
        // (session/agent/model lists, previously synchronous RPCs up to 15 s
        // on the loop thread -- ARCHITECTURE.md review) must never touch its
        // modal's Component slot from that thread; this is the loop-thread
        // side of that handoff.
        std::function<void()> poll_pickers;
        // NOT owned -- see the file comment on declaration ordering.
        std::map<std::string, TabUi>& tab_ui;

        // Part 3.2: the ChatView construction, and the permission/
        // confirmation modal rebuild-on-new-request logic, both moved here
        // from FrameRenderer::Render() -- see tab_ui.cpp's MaintainModals
        // for why (Render() must stay a pure read; deciding "is this a NEW
        // request" and rebuilding a modal component are event-path
        // decisions, not render-path ones).
        const Theme& theme;
        plugins::MessageRendererRegistry& message_registry;
        plugins::ToolResultRendererRegistry& renderer_registry;
        plugins::DisplayRendererRegistry& display_registry;
        std::shared_ptr<ftxui::Component>& permission_child;
        bool& show_permission;
        std::shared_ptr<ftxui::Component>& confirmation_child;
        bool& show_confirmation;
        std::shared_ptr<ftxui::Component>& ask_child;
        bool& show_ask;
        // The resolved chat.toggle_collapse / chat.load_older bindings, for
        // the ChatViews built here (their row hints and scrollback marker
        // name these keys).
        std::string toggle_collapse_hint = "alt+c";
        std::string load_older_hint = "pageup";
    };

    explicit TabMaintenance(Dependencies deps);

    // Gated on AppState::UiVersion(): every AppState mutation bumps it at
    // its own entry point, so "unchanged since the last pass" is a sound
    // proxy for "nothing Maintain()/HandleNotice() would read has changed
    // either" -- skips the light GetAllTabs() plus the full deep
    // GetDetails() protobuf copy otherwise paid on every event, including
    // every keystroke and mouse move that never touches AppState at all.
    void Run();

    // What the last successful pass settled on: the focused tab's TAB id
    // (AppState's tab handle -- valid for sessionless tabs too). References
    // into this object's own members -- valid as long as this TabMaintenance
    // is (RunTui declares one plain local and never relocates it, same
    // requirement as PickerController/KeyContext's dependents).
    const std::string& Focused() const { return focused_; }
    const SessionState::Snapshot& FocusedDetails() const { return focused_details_; }

private:
    SessionState::Snapshot Maintain(const std::string& tab_id, SessionState::Snapshot snap);
    void HandleNotice(const SessionState::Snapshot& snap);
    // The permission/confirmation modals: rebuilt when the focused tab's
    // outstanding request changes, shown while unanswered. Only called when
    // `focused_` is non-empty (Run() resets both flags to false itself in
    // the no-tab case, mirroring Render()'s old two-branch structure).
    void MaintainModals(const SessionState::Snapshot& snap);

    Dependencies deps_;
    std::string focused_;
    SessionState::Snapshot focused_details_;
    // Which tab's request currently occupies each SHARED modal slot (see
    // MaintainModals): the rebuild-on-new-request-id guard is keyed per tab
    // in TabUi, but permission_child/confirmation_child/ask_child are each
    // ONE slot for the whole app. Without this, focus moving A -> B -> A
    // left A's un-rebuilt guard reading "unchanged" while the shared slot
    // still held B's (possibly already-answered) modal -- A's own request
    // was never displayed, and the modal that WAS shown answered the wrong
    // session.
    std::string permission_slot_tab_;
    std::string confirmation_slot_tab_;
    std::string ask_slot_tab_;
    // Sentinel, not 0: forces the first call (before the loop starts) to
    // run regardless of whatever OpenTab's own bumps left UiVersion at.
    uint64_t last_ui_version_ = static_cast<uint64_t>(-1);
};

}  // namespace minimal::client
