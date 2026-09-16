#pragma once

// The main frame's Renderer body, factored out of RunTui (app.cpp):
// session_view's lambda, assembling the tab bar, the focused tab's body
// (chat surface / start-view banner / ChatView), the permission and
// confirmation modal rebuild-on-new-request logic, the running-tools panel,
// the prompt editor, the status bar, and plugin panel views.
//
// FrameRenderer::Render() is called from an ftxui::Renderer callback and
// must stay side-effect-free and non-blocking with respect to NETWORK/RPC
// work -- exactly the contract session_view's lambda already documented.
// Part 3.2 moved the permission/confirmation modal rebuild-on-new-request
// logic, and the primary ChatView/streaming-view construction, into
// TabMaintenance (view/tab_ui.hpp) -- both are event-path decisions
// ("is this a NEW request", "has this tab been seen before"), not render-
// path ones. Render() keeps ONE narrow exception: a null-guarded fallback
// construction of ui.chat, for the documented race where
// TabMaintenance::Maintain() returns early before ever seeing this tab (see
// its own comment) -- a lazy idempotent construct-if-null is not the same
// risk class as the modal rebuild-and-flip logic that moved out entirely.
// ui.live_text_streams (track C4) needs no such fallback: ChatView::Render
// defensively skips any live_segments index it has no slot for.

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

#include "minimal/client/render/theme.hpp"
#include "minimal/client/ui_config.hpp"
#include "minimal/client/state.hpp"
#include "minimal/client/status_format.hpp"
#include "minimal/client/view/tab_ui.hpp"

namespace minimal::client {

class FrameRenderer {
public:
    struct Dependencies {
        AppState& app;
        const Theme& theme;

        // TabMaintenance's settled output for this pass -- references into
        // TabMaintenance's own members (see its Focused()/FocusedDetails()),
        // valid as long as that object is (RunTui's whole lifetime).
        const std::string& focused;
        const SessionState::Snapshot& focused_details;

        // NOT owned -- see view/tab_ui.hpp's file comment on why the map
        // itself stays a plain RunTui local.
        std::map<std::string, TabUi>& tab_ui;

        plugins::StartViewRegistry& start_view_registry;
        plugins::MessageRendererRegistry& message_registry;
        plugins::ToolResultRendererRegistry& renderer_registry;
        plugins::DisplayRendererRegistry& display_registry;
        plugins::StatusSegmentRegistry& status_registry;
        plugins::MarkdownEngine& engine;

        bool& show_panel;
        int panel_height;   // constant for now; a resize handle is future polish (ARCHITECTURE.md)
        // ui.json's tools_panel_stdout_buffer_lines, or the built-in default
        // (100) -- already resolved by the caller (RunTui), same "0 means
        // default" convention AppState::SetUiBounds uses.
        int tools_panel_stdout_buffer_lines;
        ftxui::Component editor;   // shared_ptr: cheap to copy, same ComponentBase as RunTui's own

        const std::vector<StatusSegment>& status_format;
        std::vector<plugins::PluginViewEntry>& plugin_views;

        // The resolved chat.toggle_collapse binding spec, passed through to
        // ChatView so row-renderer hints follow the user's keymap instead of
        // a hardcoded "alt+c".
        std::string toggle_collapse_hint = "alt+c";
        // Same rule for the other bindings the frame names in its own text:
        // the empty state's session.picker, the chat's chat.load_older marker.
        std::string session_picker_hint = "ctrl+o";
        std::string load_older_hint = "pageup";
        std::string scroll_bottom_hint = "pagedown";   // the "new output below" line
        // ui.json's fold_thinking/fold_tools (kFoldDefaultAuto when unset),
        // seeded into the fallback ChatView built here -- the startup fold
        // tier IsCollapsed consults below the global collapse choice.
        int fold_thinking_default = kFoldDefaultAuto;
        int fold_tool_default = kFoldDefaultAuto;
    };

    explicit FrameRenderer(Dependencies deps);

    ftxui::Element Render();

private:
    // Start views: what renders while a session (or the app itself) has no
    // content yet -- the plugin-owned banner slot. All registered views
    // render, stacked in registration order; a THROWING view renders
    // nothing for this frame (the same containment as panel views), never
    // fatal. An empty session_id is the app-level empty state (no tab open).
    ftxui::Elements RenderStartViews(const std::string& session_id);

    Dependencies deps_;

    // Tools-panel render cache: RenderToolsPanel's input fetches every tab's
    // tool rows (state.hpp's copy-on-publish `buffered_output` -- cheap since
    // the buffer became a shared pointer, but still an O(rows) rebuild), so
    // it must not redo that on frames where nothing the panel shows moved.
    // Keyed on what the panel ACTUALLY renders, not on UiVersion: UiVersion
    // moves on every wire event -- every text/thinking delta included -- and
    // with it the panel used to rebuild (and deep-copy tool output) on
    // streaming frames that changed nothing here. The key:
    //   - AppState::ToolsVersion(): any tab's tool-row/output/rename change;
    //   - the tab count: rows appear/disappear with tabs, which bump no
    //     per-session version;
    //   - Theme::epoch(): the panel bakes colors into its Elements at build
    //     time, so the /theme swap must force one rebuild.
    // A missed-bump race (a change landing between key read and rebuild) is
    // harmless: the key only gates a cache, and the next differing key
    // rebuilds with the newer data.
    struct PanelCacheKey {
        uint64_t tools_version;
        std::size_t tab_count;
        uint64_t theme_epoch;
        bool operator==(const PanelCacheKey&) const = default;
    };
    ftxui::Element tools_panel_cache_;
    PanelCacheKey tools_panel_cache_key_{};
    bool tools_panel_cache_valid_ = false;
};

}  // namespace minimal::client
