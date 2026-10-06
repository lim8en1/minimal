#pragma once

// What core still does for the pickers, now that the pickers themselves are
// plugin views ("sessions", "subagents", "agents", "models",
// "subagent-models"): the orchestration behind the HostActions verbs they
// call -- staging a tab's desired agent or model, opening a stored or a
// subagent session -- and the first-message flow that needs a choice made
// before the message can create its session (EnsureSelectionThen), which
// opens those views by role and waits on them.
//
// ASYNC: the RPCs here (ListAgentPresets/ListModels for the first-message
// check, AttachTab/ResumeTab/OpenTranscriptTab on open) run on AppState's
// RPC worker via RunOnRpcQueue, never the FTXUI loop thread. Their results
// come back to the loop thread through QueueApply, drained by Poll() from
// TabMaintenance::Run().

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "minimal/client/session_client.hpp"
#include "minimal/client/state.hpp"

namespace minimal::client {

class PickerController {
public:
    struct Dependencies {
        SessionClient& client;
        AppState& app;
        // The desired-state choices stage onto the FOCUSED tab -- which may
        // not EXIST: a bare /agent typed on the empty startup app opens its
        // view with zero tabs, and staging onto FocusedTabId() then silently
        // dropped the choice, so the first message created the session on
        // the configured default instead. Same action typing takes: the
        // focused tab, or a fresh sessionless one.
        std::function<std::string()> focused_or_new_tab;
        // Shows the view registered under a role id ("agents", "models":
        // the server's view_hint names) -- a plugin's picker. False, with a
        // notice saying so, when no view answers to it.
        std::function<bool(const std::string& role)> open_role_view;
    };

    explicit PickerController(Dependencies deps);

    // The first message on a sessionless tab: when the tab has no agent (or
    // no model) chosen and the server has no default for it -- several to
    // choose from and no usable settings.json value -- opens the "agents"
    // view, then the "models" view, and runs `send` once both are chosen.
    // The picks are staged like any choice, so they are remembered for the
    // project. A view closed without a choice runs `cancel` instead.
    // Everything chosen, a default available, or an already-sessioned tab:
    // `send` at once. Loop thread only.
    void EnsureSelectionThen(const std::string& tab, std::function<void()> send,
                             std::function<void()> cancel);

    // The bare /agent and /model (and their keys): the "agents" / "models"
    // role views.
    void OpenAgentPicker();
    void OpenModelPicker();

    // HostActions, from the role views: stage the tab's desired agent or
    // model (with the tab notice), then continue a waiting first message.
    void ChooseAgent(const std::string& name);
    void ChooseModel(const std::string& ref);
    void ChooseDefaultModel(const std::string& default_ref);
    // `/subagent-model <ref|inherit>` on the focused tab.
    void SetSubagentModel(const std::string& ref);
    // HostActions::OpenSession / OpenSubagent (see registries.hpp).
    void OpenSession(const std::string& session_id, bool is_live);
    void OpenSubagent(const std::string& session_id, const std::string& parent, bool is_live);

    // Every pump pass, with the plugin modal showing ("" = none): a waiting
    // first message whose role view is gone without a choice is cancelled
    // (Escape closes a plugin modal in the host, unseen by the plugin).
    void WatchPendingView(const std::string& shown_modal);

    // Drains completed async work onto the loop thread. Call every pass,
    // unconditionally -- cheap (one lock, an empty check) when nothing is
    // pending. See TabMaintenance::Run.
    void Poll();

private:
    // Queues `apply` to run on the LOOP THREAD from the next Poll(), and
    // wakes the pump so that Poll() actually gets called.
    void QueueApply(std::function<void()> apply);
    // A live session attaches, a stored one resumes (see the definition for
    // why the split is load-bearing). RPC worker.
    std::string AttachOrResume(const std::string& session_id, bool is_live);

    // A first message waiting on EnsureSelectionThen's views.
    struct PendingSelection {
        std::string tab;
        std::function<void()> send;
        std::function<void()> cancel;
        bool need_agent = false;
        bool need_model = false;
        std::string awaiting;  // the role view it waits on ("" = none open)
    };
    // After a choice (`chosen`) or a view closed without one: opens the next
    // view still needed, or sends / cancels the message.
    void ContinuePendingSelection(bool chosen);
    void OpenRole(const std::string& role);

    Dependencies deps_;
    std::optional<PendingSelection> pending_selection_;
    std::mutex async_mtx_;
    std::vector<std::function<void()>> pending_apply_;
};

}  // namespace minimal::client
