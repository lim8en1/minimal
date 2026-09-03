#pragma once

// The session picker and agent preset picker's data + fetch/rebuild/open
// flows, factored out of RunTui (app.cpp). Both pickers follow the same
// shape RunTui's own comments already named: "modal over data it does not
// own" -- picker_data/agent_picker_presets are the fetched list, a mutating
// action (resume/delete/new) closes the modal, and toggling all-dirs or
// loading more re-fetches and rebuilds the component in place.
//
// PickerController owns that state privately (nothing outside this class
// read picker_data or agent_picker_presets in the inline code either) and
// writes the rebuilt component into the SAME slot Modal() reads from in
// RunTui's frame assembly -- `picker_slot`/`agent_picker_slot` are
// references to shared_ptr<Component> locals RunTui still owns and passes
// to MakeModalHost itself; this class only ever assigns *through* them
// (`*picker_slot = ...`), the same as the inline closures did.
//
// Still SYNCHRONOUS (ListSessions/ListAgentPresets block the calling
// thread), unchanged from the inline code -- making these async is a UX
// fix (ARCHITECTURE.md review, Part 4), deliberately not bundled into this
// structural move.

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <ftxui/component/component.hpp>

#include "minimal/client/render/theme.hpp"
#include "minimal/client/session_client.hpp"
#include "minimal/client/state.hpp"
#include "minimal/v1/session_service.pb.h"

namespace minimal::client {

class PickerController {
public:
    struct Dependencies {
        SessionClient& client;
        AppState& app;
        const Theme& theme;

        // Modal slots: `*picker_slot = MakeSessionPicker(...)`, mirroring
        // every other "modal over a shared_ptr<Component> slot" in RunTui
        // (permission_child, confirmation_child, ...).
        std::shared_ptr<ftxui::Component>& picker_slot;
        std::shared_ptr<ftxui::Component>& agent_picker_slot;
        std::shared_ptr<ftxui::Component>& model_picker_slot;

        bool& show_picker;
        bool& show_agent_picker;
        bool& show_model_picker;

        // The session picker's delete flow stages its OWN confirmation
        // layer (styled like the quit one) rather than deleting inline.
        std::string& staged_delete_id;
        bool& show_delete_confirm;

        // T8: the picker's "+ new session" row reaches the same OpenTab
        // path typing on an empty app does. Left as a callback (not a
        // method here) because it belongs to session-lifecycle code this
        // class has no other reason to know about.
        std::function<void()> open_new_session;
    };

    explicit PickerController(Dependencies deps);

    // The picker-open flow, shared by the keybinding and by the /sessions
    // view_hint. T8: always a fresh first page in the project's own
    // directory.
    void OpenSessionPicker();

    // Shared by the keybinding and by the bare `/agent` view_hint. One
    // fresh fetch each time: the preset list is small, no pagination state
    // to preserve across opens.
    void OpenAgentPicker();

    // Shared by the bare `/model` view_hint and the picker's own ctrl+r.
    // The bare open lists what the server already knows (static models plus
    // any cached discovery); `refresh` re-fetches every discover-entry's
    // catalog first, which can take seconds and blocks the calling thread --
    // acceptable for an explicit user action, the same bounded-pause trade
    // every picker fetch here makes.
    void OpenModelPicker(bool refresh);

private:
    void FetchPickerPage(bool reset);
    void RebuildPickerComponent();
    void RebuildAgentPickerComponent();
    void RebuildModelPickerComponent();

    // The picker's open action: ATTACH for a live row, RESUME otherwise --
    // see the definition for why the split is load-bearing.
    std::string OpenPickedSession(const std::string& session_id);

    // T8: the picker's raw accumulated list, in server delivery order
    // (updated_at DESC, id ASC per session_service.proto). Filter/sort are
    // pure DISPLAY transforms MakeSessionPicker applies fresh -- this stays
    // in fetch order so "load more"'s cursor always derives from the last
    // element the SERVER actually sent, never from whatever the current
    // sort happens to show last.
    struct PickerData {
        std::vector<v1::SessionSummary> sessions;
        bool include_all_dirs = false;
        bool has_more = false;   // the last fetch returned a full page
    };

    Dependencies deps_;
    PickerData picker_data_;
    // The agent picker's list: fetched whole on each open (no pagination,
    // no sort -- agents/*.md is small and has no "recent" axis).
    std::vector<v1::AgentPresetSummary> agent_picker_presets_;
    // The model picker's list: fetched whole on each open (refs are sorted
    // server-side; grouping is a pure display transform). No pagination --
    // even a large discovered catalog is one response of small rows.
    std::vector<v1::ModelSummary> model_picker_models_;
};

}  // namespace minimal::client
