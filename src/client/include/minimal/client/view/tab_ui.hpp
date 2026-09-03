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

struct TabUi {
    std::unique_ptr<plugins::MarkdownEngine::Stream> streaming;
    std::size_t streaming_seen = 0;
    std::optional<uint64_t> rendered_version;
    std::unique_ptr<GrantToggles> grants;   // per outstanding request
    std::string grants_for_request;
    // The wire confirmation modal's rebuild key, like grants_for_request:
    // the inner modal is rebuilt only when a NEW request_id arrives.
    std::string confirmation_for_request;
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
    // recreation, finalized_for_turn fires the engine's one authoritative
    // reparse.
    uint64_t streaming_turn = 0;
    bool finalized_for_turn = false;
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
        // By value, not reference: constant after bootstrap (a plugin
        // claimed the slot, or didn't) -- distinct from
        // AppState::EffectiveChatSurface(), which can turn null LATER if
        // the surface throws mid-render. Maintain() checks both.
        plugins::ChatSurface* chat_surface;
        plugins::ViewRegistry& view_registry;
        std::vector<plugins::PluginViewEntry>& plugin_views;
        std::shared_ptr<ftxui::Component>& plugin_modal_child;
        bool& show_plugin_modal;
        std::function<void()> open_picker;
        std::function<void()> open_agent_picker;
        std::function<void()> open_model_picker;
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
        std::shared_ptr<ftxui::Component>& permission_child;
        bool& show_permission;
        std::shared_ptr<ftxui::Component>& confirmation_child;
        bool& show_confirmation;
    };

    explicit TabMaintenance(Dependencies deps);

    // Gated on AppState::UiVersion(): every AppState mutation bumps it at
    // its own entry point, so "unchanged since the last pass" is a sound
    // proxy for "nothing Maintain()/HandleNotice() would read has changed
    // either" -- skips the light GetAllTabs() plus the full deep
    // GetDetails() protobuf copy otherwise paid on every event, including
    // every keystroke and mouse move that never touches AppState at all.
    void Run();

    // What the last successful pass settled on. References into this
    // object's own members -- valid as long as this TabMaintenance is
    // (RunTui declares one plain local and never relocates it, same
    // requirement as PickerController/KeyContext's dependents).
    const std::string& Focused() const { return focused_; }
    const SessionState::Snapshot& FocusedDetails() const { return focused_details_; }

private:
    SessionState::Snapshot Maintain(const std::string& session_id, SessionState::Snapshot snap);
    void HandleNotice(const SessionState::Snapshot& snap);
    // The permission/confirmation modals: rebuilt when the focused tab's
    // outstanding request changes, shown while unanswered. Only called when
    // `focused_` is non-empty (Run() resets both flags to false itself in
    // the no-tab case, mirroring Render()'s old two-branch structure).
    void MaintainModals(const SessionState::Snapshot& snap);

    Dependencies deps_;
    std::string focused_;
    SessionState::Snapshot focused_details_;
    // Sentinel, not 0: forces the first call (before the loop starts) to
    // run regardless of whatever OpenTab's own bumps left UiVersion at.
    uint64_t last_ui_version_ = static_cast<uint64_t>(-1);
};

}  // namespace minimal::client
