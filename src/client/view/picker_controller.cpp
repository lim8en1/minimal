#include "minimal/client/view/picker_controller.hpp"

#include <optional>
#include <utility>

#include "minimal/common/logger.hpp"

namespace minimal::client {

PickerController::PickerController(Dependencies deps) : deps_(std::move(deps)) {}

// The picker's open action, decided per ROW: a LIVE session (is_live -- a
// Session object still exists server-side, attached or detached) ATTACHES to
// the running instance, the way the row's label has always claimed; a stored
// one RESUMES from the transcript. Routing a live session through ResumeTab
// instead made the server refuse the resume with "already open" -- the exact
// "resume failed" a user saw after quitting with ctrl+c (which detaches, and
// leaves every session live) and reopening the client.
//
// Runs on AppState's RPC worker (see the file comment) -- AttachTab/ResumeTab
// carry 5 s deadlines, which used to block the loop thread on every open.
// `is_live` is resolved by the CALLER, on the loop thread, before this is
// queued -- `is_live` false (not found in the fetched pages: a stale picker,
// or the picker was never asked) falls straight through to resume, the only
// meaningful try when there is nothing to attach to that the list knows of.
std::string PickerController::AttachOrResume(const std::string& session_id, bool is_live) {
    if (is_live) {
        if (std::string attached = deps_.app.AttachTab(session_id); !attached.empty()) {
            return attached;
        }
        // is_live was stale -- the session ended between the list fetch and
        // this click (or this client already had the tab, which both paths
        // handle). Resume is the remaining option, and its failure is
        // visible (an Error-state tab), never silent.
    }
    return deps_.app.ResumeTab(session_id);
}

// HostActions::OpenSession, from the "sessions" view. Off the loop thread:
// AttachTab/ResumeTab carry 5 s deadlines. A PAST session opens into its own
// root group; it is not a child of whatever is focused. AttachOrResume never
// returns "".
void PickerController::OpenSession(const std::string& session_id, bool is_live) {
    deps_.app.RunOnRpcQueue([this, session_id, is_live] {
        deps_.app.SetFocusedTab(AttachOrResume(session_id, is_live));
    });
}

// HostActions::OpenSubagent, from the "subagents" view.
void PickerController::OpenSubagent(const std::string& session_id, const std::string& parent,
                                    bool is_live) {
    deps_.app.RunOnRpcQueue([this, session_id, is_live, parent] {
        // Already open -> focus it. Everything
        // downstream (AttachTab's duplicate rule,
        // OpenTranscriptTab's) agrees: one tab per
        // session, the existing one is the answer.
        for (const auto& tab : deps_.app.GetAllTabs()) {
            if (tab.session_id == session_id) {
                deps_.app.SetFocusedTab(tab.tab_id);
                return;
            }
        }
        if (is_live) {
            // Group under the parent's root -- the
            // same ordinal lookup OnSubagentSpawned
            // does, so the attached child takes its
            // "↳" place in the group.
            int64_t root_ordinal = -1;
            for (const auto& tab : deps_.app.GetAllTabs()) {
                if (tab.session_id == parent) {
                    root_ordinal = tab.root_ordinal;
                    break;
                }
            }
            if (std::string attached =
                    deps_.app.AttachTab(session_id, root_ordinal);
                !attached.empty()) {
                deps_.app.SetFocusedTab(attached);
                return;
            }
            // is_live went stale between the fetch
            // and this click (the child's turn just
            // ended): the stored transcript is
            // what remains to show -- fall through.
        }
        if (std::string opened =
                deps_.app.OpenTranscriptTab(session_id, parent);
            !opened.empty()) {
            deps_.app.SetFocusedTab(opened);
        }
    });
}

void PickerController::QueueApply(std::function<void()> apply) {
    {
        std::lock_guard<std::mutex> lock(async_mtx_);
        pending_apply_.push_back(std::move(apply));
    }
    // Same counter every ordinary AppState mutation bumps -- wakes the 20 Hz
    // pump so a frame lands with Poll() able to drain this.
    deps_.app.NotifyExternalUpdate();
}

void PickerController::Poll() {
    std::vector<std::function<void()>> ready;
    {
        std::lock_guard<std::mutex> lock(async_mtx_);
        if (pending_apply_.empty()) return;
        ready.swap(pending_apply_);
    }
    for (auto& apply : ready) apply();   // loop thread only, from here on
}

// The desired-state choices -- the view registered as "agents" / "models"
// (a plugin's) calls these through HostActions. The DESIRED-STATE path,
// identical on every tab: the choice sets the tab's desired preset or model,
// which rides CreateSessionRequest on a sessionless tab (the first message
// creates the session on it) and converges at the next turn start on a
// sessioned one. No command is sent, and no session is minted just to
// switch. focused_or_new_tab, not FocusedTabId: on the empty startup app
// (zero tabs -- how a bare /agent gets here) there is nothing focused, and
// the choice must land on the tab the user's first message will create.
void PickerController::ChooseAgent(const std::string& name) {
    if (const std::string tab = deps_.focused_or_new_tab(); !tab.empty()) {
        deps_.app.StageAgentPreset(tab, name);
        deps_.app.TabNotice(tab, "agent '" + name + "' will be used on your next message");
    }
    ContinuePendingSelection(/*chosen=*/true);
}

void PickerController::ChooseModel(const std::string& ref) {
    if (const std::string tab = deps_.focused_or_new_tab(); !tab.empty()) {
        deps_.app.StageModel(tab, ref);
        deps_.app.TabNotice(tab, "model '" + ref + "' will be used on your next message");
    }
    ContinuePendingSelection(/*chosen=*/true);
}

// Back to the default model (settings.json default_model, or the only
// model): clears the tab's own choice. `default_ref` is what the server
// named the default, for the status bar until the session confirms it.
void PickerController::ChooseDefaultModel(const std::string& default_ref) {
    if (const std::string tab = deps_.focused_or_new_tab(); !tab.empty()) {
        deps_.app.StageModel(tab, default_ref, /*is_default=*/true);
        deps_.app.TabNotice(tab, "the default model will be used on your next message");
    }
    ContinuePendingSelection(/*chosen=*/true);
}

// NOT desired state: the subagent model is a session's own setting, so it
// is the /subagent-model command (whose result surfaces as the tab notice).
void PickerController::SetSubagentModel(const std::string& ref) {
    if (const std::string tab = deps_.focused_or_new_tab(); !tab.empty()) {
        deps_.app.SubmitCommand(tab, "/subagent-model " + ref);
    }
}

void PickerController::OpenAgentPicker() { OpenRole("agents"); }

void PickerController::OpenModelPicker() { OpenRole("models"); }

void PickerController::OpenRole(const std::string& role) {
    if (!deps_.open_role_view(role)) {
        // Nothing to choose with (open_role_view said so in a notice): a
        // first message waiting on a choice cannot proceed.
        if (pending_selection_) ContinuePendingSelection(/*chosen=*/false);
        return;
    }
    if (pending_selection_) pending_selection_->awaiting = role;
}

void PickerController::WatchPendingView(const std::string& shown_modal) {
    // The awaited view went away -- Escape (the host's), or another modal
    // replaced it -- with no choice having moved the selection on: the
    // message is not sent.
    if (!pending_selection_ || pending_selection_->awaiting.empty()) return;
    if (shown_modal == pending_selection_->awaiting) return;
    ContinuePendingSelection(/*chosen=*/false);
}

void PickerController::EnsureSelectionThen(const std::string& tab, std::function<void()> send,
                                           std::function<void()> cancel) {
    const auto selection = deps_.app.SelectionFor(tab);
    const bool has_agent = !selection.agent.empty();
    const bool has_model = !selection.model.empty() && !selection.model_default;
    if (!selection.exists || selection.sessioned || (has_agent && has_model)) {
        send();
        return;
    }
    // Whether the server has a default is its answer (settings, or the only
    // option), asked fresh: discovery may have filled the model list since.
    const std::string root = selection.project_root;
    deps_.app.RunOnRpcQueue(
        [this, tab, root, has_agent, has_model, send = std::move(send),
         cancel = std::move(cancel)]() mutable {
            std::optional<v1::ListAgentPresetsResponse> agents;
            std::optional<v1::ListModelsResponse> models;
            if (!has_agent) {
                auto result = deps_.client.ListAgentPresets(root);
                if (result.status.ok()) agents = std::move(result.response);
            }
            if (!has_model) {
                auto result = deps_.client.ListModels({});
                if (result.status.ok()) models = std::move(result.response);
            }
            QueueApply([this, tab, agents = std::move(agents), models = std::move(models),
                        send = std::move(send), cancel = std::move(cancel)]() mutable {
                PendingSelection pending{tab, std::move(send), std::move(cancel)};
                // Ask only when there is a choice and no default. A failed
                // listing, or nothing to choose from, is left to the create's
                // own error, which names the fix.
                pending.need_agent =
                    agents && agents->default_agent_preset().empty() && agents->presets_size() > 0;
                pending.need_model =
                    models && models->default_model().empty() && models->models_size() > 0;
                if (!pending.need_agent && !pending.need_model) {
                    pending.send();
                    return;
                }
                if (pending_selection_ && pending_selection_->cancel) pending_selection_->cancel();
                pending_selection_ = std::move(pending);
                ContinuePendingSelection(/*chosen=*/true);
            });
        },
        /*must_run=*/true, AppState::RpcLane::kBulk);
}

void PickerController::ContinuePendingSelection(bool chosen) {
    if (!pending_selection_) return;
    pending_selection_->awaiting.clear();
    if (!chosen) {
        auto cancel = std::move(pending_selection_->cancel);
        pending_selection_.reset();
        if (cancel) cancel();
        return;
    }
    const auto& pending = *pending_selection_;
    const auto selection = deps_.app.SelectionFor(pending.tab);
    if (pending.need_agent && selection.agent.empty()) {
        deps_.app.TabNotice(pending.tab,
                            "choose an agent for this project (remembered for next time) -- "
                            "your message is sent once you have");
        OpenRole("agents");
        return;
    }
    if (pending.need_model && (selection.model.empty() || selection.model_default)) {
        deps_.app.TabNotice(pending.tab,
                            "choose a model for this project (remembered for next time) -- "
                            "your message is sent once you have");
        OpenRole("models");
        return;
    }
    auto send = std::move(pending_selection_->send);
    pending_selection_.reset();
    send();
}

}  // namespace minimal::client
