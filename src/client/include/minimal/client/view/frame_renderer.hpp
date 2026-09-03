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
// construction of ui.chat/ui.streaming, for the documented race where
// TabMaintenance::Maintain() returns early before ever seeing this tab (see
// its own comment) -- a lazy idempotent construct-if-null is not the same
// risk class as the modal rebuild-and-flip logic that moved out entirely.

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

#include "minimal/client/render/theme.hpp"
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
};

}  // namespace minimal::client
