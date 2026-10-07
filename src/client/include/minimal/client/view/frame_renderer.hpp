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
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <functional>

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

#include "minimal/client/sdk/theme.hpp"
#include "minimal/client/ui_config.hpp"
#include "minimal/client/state.hpp"
#include "minimal/client/status_layout.hpp"
#include "minimal/client/view/tab_ui.hpp"
#include "minimal/client/policy.hpp"

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
        const plugins::ChatFrame& focused_frame;   // ChatFrameFrom(focused_details)

        // NOT owned -- see view/tab_ui.hpp's file comment on why the map
        // itself stays a plain RunTui local.
        std::map<std::string, TabUi>& tab_ui;

        plugins::StartViewRegistry& start_view_registry;
        plugins::StatusSegmentRegistry& status_registry;
        const plugins::TabBarRegistry& tab_bar_registry;   // empty: no tab bar

        int panel_height;   // rows for a plugin panel's content (ui.json panel_height)
        ftxui::Component editor;   // shared_ptr: cheap to copy, same ComponentBase as RunTui's own

        const std::vector<StatusSegment>& status_format;
        // The segment ids status_format names: the only segments rendered.
        const std::set<std::string>& status_ids_in_format;
        std::vector<plugins::PluginViewEntry>& plugin_views;

        // The binding the empty state names (the user's keymap decides it).
        std::string session_picker_hint = "ctrl+o";
        // Builds a tab's chat pane when a frame reaches a tab the maintenance
        // pass has not processed yet (TabMaintenance::EnsureChatPane).
        std::function<void(const std::string& tab_id, TabUi& ui)> ensure_chat_pane;
    };

    explicit FrameRenderer(Dependencies deps);

    ftxui::Element Render();

    // The tab under screen (x, y) in the last laid-out frame ("" = none):
    // a click on the tab bar focuses that tab.
    std::string TabAt(int x, int y) const { return tab_hits_.TabAt(x, y); }

    // A plugin object is gone (a closed tab's chat pane): its throw count
    // goes too, so an object later allocated at the same address starts
    // clean.
    void ForgetPluginCall(const void* key) { plugin_throws_.erase(key); }

private:
    // Start views: what renders while a session (or the app itself) has no
    // content yet -- the plugin-owned banner slot. All registered views
    // render, stacked in registration order; a THROWING view renders
    // nothing for this frame (the same containment as panel views), never
    // fatal. An empty session_id is the app-level empty state (no tab open).
    ftxui::Elements RenderStartViews(const std::string& session_id);

    Dependencies deps_;

    // Per-frame plugin calls (start views, panels, status segments) run in
    // GuardPluginCall: a throw is contained, logged ONCE (not at frame rate
    // -- a broken segment used to write an error line per repaint), and
    // after kMaxPluginThrows consecutive throws the call is skipped for the
    // rest of the session, the way a throwing chat surface is disabled.
    // Returns whether `call` ran and returned normally. `key` is the plugin
    // object called (a segment, a view, a tab's chat pane): free to compare,
    // where a string key cost two concatenations per segment per frame.
    // `kind` and `name` make the log's noun ("status segment 'todo'"), only
    // built when something threw.
    template <typename Call>
    bool GuardPluginCall(const void* key, std::string_view kind, std::string_view name,
                         Call&& call) {
        int& throws = plugin_throws_[key];
        if (throws < 0) return false;   // disabled after repeated throws
        try {
            call();
            throws = 0;
            return true;
        } catch (const std::exception& e) {
            NotePluginThrow(throws, kind, name, e.what());
        } catch (...) {
            NotePluginThrow(throws, kind, name, "a non-standard exception");
        }
        return false;
    }
    void NotePluginThrow(int& throws, std::string_view kind, std::string_view name,
                         std::string_view error);
    std::unordered_map<const void*, int> plugin_throws_;
    plugins::TabBarHits tab_hits_;   // the tab bar's cells, as last laid out

};

}  // namespace minimal::client
