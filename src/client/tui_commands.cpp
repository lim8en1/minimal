// THE CLIENT-SIDE /-COMMANDS (command_router.hpp): one table, listing and
// behavior together. A command with a summary is the client's own and is
// offered in /-autocomplete; one without is a server command whose `run`
// intercepts particular forms and returns false for the rest.
//
// /rewind and /redo are client-native ClientEvents, not server Commands (the
// client resolves "N user turns back" to a seq itself); /new, /detach-session
// and /theme are client operations the server has no part in; /pause and
// /resume send SetPausedEvent directly (a /-command would queue for the worker
// and land only at a turn boundary -- the delay pause exists to avoid). Bare
// /help, /keymap, /agent, /model, /subagent-model and (sessionless) /sessions
// open client-owned overlays with no round trip.

#include <string>

#include "client_entry.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/common/model_ref.hpp"
#include "tui_app.hpp"

namespace minimal::client::app {

void TuiApp::RegisterClientCommands() {
    CommandRouter& router = command_router_;

    for (const bool pause : {true, false}) {
        router.Add({pause ? "pause" : "resume",
                    pause ? "pause the agent (stops after its current tool call)"
                          : "resume a paused session (continues from its last tool result)",
                    false, true, [this, pause](const CommandInvocationText&) {
            // RequestPaused reads the focused TAB's snapshot and targets its
            // session; the confirming metadata event refreshes the status
            // bar. An explicit /resume with nothing queued kicks the seedless
            // continuation server-side (Session::SetPaused).
            const auto toggled = app_->RequestPaused(pause);
            if (toggled == AppState::PauseToggle::NoSession) {
                FocusedNotice(std::string(pause ? "pause" : "resume") +
                              " needs a session; send a message to start one first");
            } else if (toggled == AppState::PauseToggle::AlreadyPaused ||
                       toggled == AppState::PauseToggle::AlreadyRunning) {
                FocusedNotice(pause ? "already paused" : "not paused");
            } else {
                FocusedNotice(pause ? "pausing -- the agent stops after its current tool call"
                                    : "resumed");
            }
            return true;
        }});
    }
    router.Add({"redo", "restore the most recently rewound turn", false, true,
                [this](const CommandInvocationText&) {
        const std::string sid = app_->FocusedSessionId();
        // Refused VISIBLY on a sessionless tab: SendRewind with no session
        // silently did nothing.
        if (sid.empty()) {
            FocusedNotice("/redo needs a session; send a message to start one first");
        } else {
            app_->SendRewind(sid, std::nullopt, /*redo=*/true);
        }
        return true;
    }});
    router.Add({"rewind", "undo the most recent user turn (or N of them)", false, true,
                [this](const CommandInvocationText& parsed) {
        const std::string sid = app_->FocusedSessionId();
        if (sid.empty()) {
            FocusedNotice("/rewind needs a session; send a message to start one first");
            return true;
        }
        int n = 1;   // default: undo the most recent user turn
        if (!parsed.raw.empty()) {
            // Reject a partially-numeric arg ("2x" -- stoi stops at the first
            // non-digit) and a non-positive one, rather than acting on a
            // depth the user never typed.
            bool valid = true;
            try {
                std::size_t consumed = 0;
                n = std::stoi(parsed.raw, &consumed);
                valid = consumed == parsed.raw.size() && n > 0;
            } catch (const std::exception&) {
                valid = false;   // "/rewind all", overflow, no digits at all
            }
            if (!valid) {
                FocusedNotice("/rewind needs a positive number of turns, e.g. /rewind 2");
                return true;
            }
        }
        // nullopt covers both "fewer than n user turns in the loaded window"
        // and FindRewindTargetSeq's refusal on a trimmed window (its front is
        // no longer the true newest message).
        if (const auto seq = app_->FindRewindTargetSeq(sid, n)) {
            app_->SendRewind(sid, *seq, /*redo=*/false);
        } else {
            FocusedNotice("/rewind: could not find that many turns in the loaded history");
        }
        return true;
    }});
    // Always a NEW sessionless tab -- a sibling workspace, not a reuse of the
    // focused one. Nothing is created server-side until its first message.
    router.Add({"new", "start a new session", false, false, [this](const CommandInvocationText&) {
        app_->SetFocusedTab(app_->OpenTab(NewTabOptions()));
        return true;
    }});
    // A TAB operation: end the group's streams with EndSession{DETACH} and
    // erase the tabs (TabsToClose: the focused root plus its children, or the
    // whole group from a child). The sessions keep running server-side and
    // /sessions reattaches them. No confirmation: nothing is destroyed.
    router.Add({"detach-session", "detach this tab group; work keeps running, /sessions reattaches",
                false, true, [this](const CommandInvocationText&) {
        const std::string focused_tab = app_->FocusedTabId();
        if (focused_tab.empty()) return true;
        if (app_->FocusedSessionId().empty()) {
            CloseTab(focused_tab);   // nothing to detach; closing is the whole job
            return true;
        }
        const auto ids = TabsToClose(app_->GetAllTabs(), focused_tab);
        app_->EndTabSessions(ids, EndSessionMode::kDetach);
        for (const auto& id : ids) CloseTab(id);
        FocusedNotice("detached -- /sessions reattaches a live session");
        return true;
    }});
    // Decoration, not session state: works on any tab. Bare opens the picker,
    // named swaps directly.
    router.Add({"theme", "switch the UI theme", true, false,
                [this](const CommandInvocationText& parsed) {
        if (parsed.positional.empty()) {
            ClearFocusedNotice();
            OpenThemePicker();
        } else {
            ApplyTheme(parsed.positional.front());
        }
        return true;
    }});
    // THE DESIRED-STATE COMMANDS, identical on every tab: bare opens the
    // client-owned picker; named sets the tab's desired state (riding
    // CreateSessionRequest on a sessionless tab, converging at the next turn
    // on a sessioned one) -- never a direct switch, so the status bar shows
    // the fresh value at once, pending until the metadata event confirms.
    for (const bool is_agent : {true, false}) {
        router.Add({is_agent ? "agent" : "model", "", false, false,
                    [this, is_agent](const CommandInvocationText& parsed) {
            if (parsed.positional.empty()) {
                ClearFocusedNotice();
                if (is_agent) {
                    picker_controller_->OpenAgentPicker();
                } else {
                    picker_controller_->OpenModelPicker(/*refresh=*/false);
                }
                return true;
            }
            const std::string& value = parsed.positional.front();
            const std::string tab = FocusedOrNewTab();
            if (tab.empty()) return true;
            if (is_agent) {
                app_->StageAgentPreset(tab, value);
            } else if (value == "default") {
                // Back to settings.json default_model. A live session is
                // switched by the server (the ref is its to know); a
                // sessionless tab is created without a model choice.
                // Either way any earlier staged choice is dropped, so the
                // next turn does not converge back to it.
                app_->StageModel(tab, "", /*is_default=*/true);
                if (!app_->FocusedSessionId().empty()) return false;
                app_->TabNotice(tab, "the default model will be used on your next message");
                return true;
            } else {
                // Shape-checked here (the parser the headless --model flag
                // uses) so a typo stages nothing; whether it RESOLVES is
                // still the server's call.
                if (!common::SplitModelRef(value)) {
                    app_->TabNotice(tab, "'" + value +
                                             "' must be <provider>/<model> (e.g. local/qwen4) or "
                                             "'default' (or run /model with no arguments)");
                    return true;
                }
                app_->StageModel(tab, value);
            }
            app_->TabNotice(tab, std::string(is_agent ? "agent '" : "model '") + value +
                                     "' will be used on your next message");
            return true;
        }});
    }
    // NOT a desired-state command: its subject is a session's own setting.
    // Only the bare form on a sessioned tab is the client's (the picker);
    // everything else goes to the server, which validates the ref and
    // answers the sessionless refusal.
    router.Add({"subagent-model", "", false, true, [this](const CommandInvocationText& parsed) {
        if (!parsed.positional.empty() || app_->FocusedSessionId().empty()) return false;
        ClearFocusedNotice();
        picker_controller_->OpenSubagentModelPicker();
        return true;
    }});
    // Bare /sessions on a SESSIONLESS tab opens the picker outright: the
    // server's answer was only ever a view_hint pointing at it, and the round
    // trip minted a junk session. On a sessioned tab it goes to the server
    // like any other command.
    router.Add({"sessions", "", true, false, [this](const CommandInvocationText& parsed) {
        if (!parsed.positional.empty() || !app_->FocusedSessionId().empty()) return false;
        picker_controller_->OpenSessionPicker();
        return true;
    }});
    // Plugin-contributed client commands (ClientCommandRegistry), after the
    // client's own: a name the table already holds stays the client's. A
    // plugin command needing a session is refused visibly on a sessionless
    // tab, the same rule the server's catalog gets in SubmitCommand.
    for (const auto& plugin_command : env_.registries.client_commands.All()) {
        const std::string name(plugin_command->name());
        if (router.Has(name)) {
            common::DefaultLogger().warn(
                "client plugin command '/{}' is ignored: the client already has a command by "
                "that name",
                name);
            continue;
        }
        const bool needs_session = plugin_command->requires_session();
        // An unlisted command intercepts a server command's forms: no
        // summary, so the catalog keeps the server's entry for the name.
        const std::string summary =
            plugin_command->listed() ? std::string(plugin_command->summary()) : std::string();
        router.Add({name, summary, false, needs_session,
                    [this, command = plugin_command.get(), name,
                     needs_session](const CommandInvocationText& parsed) {
            if (needs_session && app_->FocusedSessionId().empty()) {
                FocusedNotice("/" + name + " needs a session; send a message to start one first");
                return true;
            }
            try {
                return command->Run(parsed.positional, parsed.raw);
            } catch (const std::exception& error) {
                FocusedNotice("/" + name + " failed: " + error.what());
            } catch (...) {
                FocusedNotice("/" + name + " failed");
            }
            return true;
        }});
    }
}

}  // namespace minimal::client::app
