// The interactive TUI (RunTui): builds the environment, the frame, the modal
// stack and the key dispatch, then runs the FTXUI loop.

#include "client_entry.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include <unistd.h>

#include <nlohmann/json.hpp>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <optional>

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include "minimal/client/chat_model.hpp"
#include "minimal/client/cli_args.hpp"
#include "minimal/client/command_catalog.hpp"
#include "minimal/client/command_router.hpp"
#include "minimal/client/keybinding_dispatch.hpp"
#include "minimal/client/keybindings.hpp"
#include "minimal/client/session_client.hpp"
#include "minimal/client/scrollback_refresh.hpp"
#include "minimal/client/server_spawn.hpp"
#include "minimal/client/status_format.hpp"
#include "minimal/client/tui_bootstrap.hpp"
#include "minimal/client/ui_config.hpp"
#include "minimal/client/utf8.hpp"
#include "minimal/client/plugins/registries.hpp"
#include "minimal/client/view/chat_view.hpp"
#include "minimal/client/view/frame_renderer.hpp"
#include "minimal/client/view/modal_host.hpp"
#include "minimal/client/view/modal_stack.hpp"
#include "minimal/client/view/prompt_editor.hpp"
#include "minimal/client/view/prompt_modal.hpp"
#include "minimal/client/view/agent_picker.hpp"
#include "minimal/client/view/commands_help.hpp"
#include "minimal/client/view/keymap_help.hpp"
#include "minimal/client/view/picker_controller.hpp"
#include "minimal/client/view/session_picker.hpp"
#include "minimal/client/view/status_bar.hpp"
#include "minimal/client/view/tab_bar.hpp"
#include "minimal/client/view/tab_ui.hpp"
#include "minimal/client/view/theme_picker.hpp"
#include "minimal/client/view/tools_panel.hpp"
#include "minimal/client/render/color_downsample.hpp"
#include "minimal/client/render/streaming_markdown_view.hpp"
#include "minimal/client/render/theme.hpp"
#include "minimal/common/abi_tag.hpp"
#include "minimal/common/config_paths.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/common/logging_setup.hpp"
#include "minimal/common/model_ref.hpp"

namespace minimal::client::app {

namespace {
// --- TUI --------------------------------------------------------------------
//
// Frame assembly (ARCHITECTURE.md "TUI layout"): tab bar / session view /
// optional running-tools panel / status bar, plus a modal layer.
//
// THE BRIDGE: gRPC reader threads never touch FTXUI objects. They mutate
// SessionState/AppState (plain, mutex-guarded state), and a 20 Hz pump posts a
// custom event into the screen loop so it redraws from a snapshot. Every
// component below READS state; nothing on the loop thread waits on the
// reader, and nothing on the reader waits on the loop. Unary RPCs that used
// to run ON the loop thread (RequestScrollback -- née RefreshScrollback,
// SuggestPaths) instead go through AppState's async RPC worker
// (rpc_queue.hpp): Submit() returns immediately, the job runs off-thread and
// publishes into SessionState/AppState the same way a reader thread does.
// That worker is joined explicitly (app->StopRpcQueue(), below, right after
// the pump) rather than left detached, for the same teardown-ordering reason
// the reader/writer threads are joined rather than detached: a job's
// completion touches state that CloseAllTabs() is about to tear down.
//
// Keybindings: every action is dispatched through keybindings.json (the file
// is the config); the table below only maps a key NAME to the byte sequences a
// terminal may send for it. A name the terminal cannot encode matches nothing,
// which is exactly what remapping exists for -- and what the startup warning
// names.

using minimal::client::AppState;
using minimal::client::GrantToggles;
using minimal::client::Keybindings;
using minimal::client::PendingConfirmation;
using minimal::client::PendingPermission;
using minimal::client::PromptEditorState;

using minimal::client::HeadlessOptions;
using minimal::client::kFoldCollapsed;   // the fold-toggle notice lines (fb06594)
using minimal::client::ResolvedKeymap;
using minimal::client::SessionState;
using minimal::client::TabUi;
using minimal::client::SessionStateKind;
using minimal::client::Theme;

// Looks up tab_id's TabUi and, if it exists and has a live ChatView yet
// (a sessionless tab doesn't), calls fn(chat_view). Returns fn's result, or a
// default-constructed one (false/nullptr/nothing) on a miss. Collapses what
// used to be nine copy-pasted `tab_ui.find(...)` guards in KeyContext's
// construction below into one-line calls, so a future fix to the lookup/guard
// itself only has to happen once.
template <typename Fn>
auto WithChat(std::map<std::string, minimal::client::TabUi>& tab_ui, const std::string& tab_id,
             Fn&& fn) {
    using Ret = std::invoke_result_t<Fn, minimal::client::ChatView&>;
    if (auto it = tab_ui.find(tab_id); it != tab_ui.end() && it->second.chat) {
        return fn(*it->second.chat);
    }
    if constexpr (!std::is_void_v<Ret>) {
        return Ret{};
    }
}

// What a quit / tab-close confirmation will act on, when answered: the
// confirm modal chooses TERMINATE (destroy the sessions and everything in
// flight) or DETACH (end only the streams; work finishes unattended and
// /sessions reattaches).
struct PendingClose {
    bool quit = false;
    // STAGING ORDER: render order -- the root FIRST, its children directly
    // after (TabsToClose walks the tab list, where children follow their
    // parent). Closing the root terminates the whole group, so the child
    // closes below land on already-terminated sessions and are no-ops; the
    // order is therefore safe either way, but these comments used to claim
    // children-first, which is not what the staging produces.
    std::vector<std::string> tab_ids;
    bool any = false;
};

}  // namespace

int RunTui(const minimal::client::ParsedArgs& args) {
    using namespace ftxui;
    namespace plugins = minimal::client::plugins;

    const std::string target = args.headless.server.empty()
                                   ? minimal::client::DefaultServerTarget()
                                   : args.headless.server;

    // The same connect (auto-spawn first, 5 s wait) as the headless paths.
    const auto client = ConnectOrReport(target, args.headless.autospawn);
    if (!client) return kUnavailable;

    // The startup --resume probe: resolve the target session BEFORE the UI is
    // built, so a bad id fails fast with a clear line instead of an empty TUI.
    // What is kept from the summary is exactly what the attach step needs:
    // liveness (attach vs revive) and the subagent refusal (a subagent's life
    // is one turn of its parent; opening one standalone has no meaning -- the
    // server refuses its resume too).
    std::optional<minimal::v1::SessionSummary> startup_resume;
    if (!args.headless.resume_session_id.empty()) {
        const auto found = client->GetSession(args.headless.resume_session_id);
        if (found.status.error_code() == grpc::StatusCode::NOT_FOUND) {
            std::fprintf(stderr, "no such session: %s\n",
                         args.headless.resume_session_id.c_str());
            std::fprintf(stderr, "list stored sessions with: minimal --list-sessions\n");
            return kUnavailable;
        }
        if (!found.status.ok()) {
            std::fprintf(stderr, "could not look up session %s: %s\n",
                         args.headless.resume_session_id.c_str(),
                         found.status.error_message().c_str());
            return kUnavailable;
        }
        if (!found.summary.parent_session_id().empty()) {
            std::fprintf(stderr, "session %s is a subagent of %s and cannot be opened "
                                 "standalone; open the parent session instead\n",
                         args.headless.resume_session_id.c_str(),
                         found.summary.parent_session_id().c_str());
            return kUnavailable;
        }
        startup_resume = found.summary;
    }

    // The /-command catalog: SEEDED here with one synchronous fetch at
    // connect (the startup project root), so the very first `/` has
    // something to show; from then on PULLED per tab scope by
    // CommandCatalogCache (built below, once `app` exists) -- every read
    // answers from the last known list and, at most every 2 s, refetches in
    // the background. So a command file added to the project mid-run
    // appears the next time autocomplete or /help opens, and a tab in a
    // different project lists that project's commands.
    const auto to_catalog_entries = [](const minimal::v1::ListCommandsResponse& response) {
        std::vector<minimal::client::CommandCatalogEntry> entries;
        for (const auto& info : response.commands()) {
            entries.push_back({info.name(), info.summary(), info.read_only(),
                               info.kind() == minimal::v1::COMMAND_KIND_PROMPT_SHORTHAND,
                               info.requires_session()});
        }
        return entries;
    };
    std::optional<std::vector<minimal::client::CommandCatalogEntry>> seed_catalog;
    {
        const auto listed = client->ListCommands(minimal::client::CurrentProjectRootOrEmpty());
        if (listed.status.ok()) seed_catalog = to_catalog_entries(listed.response);
    }
    // --- config + client plugins --------------------------------------------
    // ORDER MATTERS -- see tui_bootstrap.hpp/.cpp for the full sequence
    // (ui.json -> grammars -> plugin load -> collisions -> theme ->
    // keybindings -> status format -> highlighter/renderer/engine -> chat
    // surface). `env` MUST be a plain local, never moved/copied/returned:
    // several of its fields hold pointers into ITS OWN other fields (the
    // header's address-stability contract), so BuildTuiEnvironment fills it
    // in place rather than returning one by value.
    minimal::client::TuiEnvironment env;
    if (!minimal::client::BuildTuiEnvironment(*client, env)) {
        // Plugins loaded before the failure may already hold subscribe
        // streams on worker threads; cancel them so `env`'s teardown (its
        // feeds join those workers) cannot wait on a stream nobody ends --
        // the same step shutdown_tui takes on the normal path.
        client->CancelSubscriptions();
        return kUnavailable;
    }

    // Local aliases, same names the inline code used to declare directly:
    // every lambda below this point captures `[&]`, i.e. by ordinary lexical
    // scoping, so introducing these here (rather than rewriting every
    // capture site to say `env.theme` etc.) is what keeps the rest of this
    // function's text unchanged. `theme`/`engine` deref an optional/pointer
    // that BuildTuiEnvironment guarantees non-null on success.
    Theme& theme = *env.theme;
    plugins::MarkdownEngine& engine = *env.engine;
    plugins::ChatSurface* chat_surface = env.chat_surface;
    auto& view_registry = env.view_registry;
    auto& start_view_registry = env.start_view_registry;
    auto& renderer_registry = env.renderer_registry;
    auto& display_registry = env.display_registry;
    auto& message_registry = env.message_registry;
    auto& status_registry = env.status_registry;
    const auto& status_format = env.status_format;
    const auto& keymap = env.keymap;
    auto& plugin_views = env.plugin_views;

    auto app = std::make_shared<AppState>(client);
    app->SetPluginAttention(&env.attention_state);

    // The pull-model catalog (see the seed fetch above). Declared after
    // `app`, so destroyed before it; a fetch landing after that is dropped
    // (the cache's completion holds only a weak reference).
    minimal::client::CommandCatalogCache command_catalog_cache(
        {},   // the client's own commands: SetClientSide, once the router is built
        [app, to_catalog_entries](
            const std::string& session_id, const std::string& project_root,
            std::function<void(std::optional<minimal::client::CommandCatalogCache::Entries>)>
                done) {
            app->RequestListCommands(
                session_id, project_root,
                [done = std::move(done), to_catalog_entries](
                    std::optional<minimal::v1::ListCommandsResponse> response) {
                    if (!response) return done(std::nullopt);
                    done(to_catalog_entries(*response));
                });
        });
    // The focused tab's scope: its session, else its pending project root
    // (a tab-less app scopes like a fresh tab would, to the TUI's cwd).
    const auto command_scope = [app]() {
        auto scope = app->FocusedCommandScope();
        if (scope.session_id.empty() && scope.project_root.empty()) {
            scope.project_root = minimal::client::CurrentProjectRootOrEmpty();
        }
        return scope;
    };
    const minimal::client::CommandCatalogSource current_catalog =
        [&command_catalog_cache, command_scope]() {
            const auto scope = command_scope();
            return command_catalog_cache.Get(scope.session_id, scope.project_root);
        };
    app->SetChatSurface(chat_surface);
    // THE STARTUP RESUME, on the probed session (probe above, before the UI
    // was built). LIVE opens the whole tab group -- the session plus every
    // live subagent child (AttachSessionGroup); anything else REVIVES the
    // stored transcript into a fresh root group (ResumeTab), the picker's own
    // stale-liveness fallthrough. "" from the live path means the session
    // ended between probe and attach; resume is then the remaining option,
    // and its failure is visible as an Error tab, never silent. Either way
    // the tab focuses itself (focus-if-empty), so no separate SetFocusedTab
    // is needed here.
    if (startup_resume) {
        const std::string session_id = startup_resume->session_id();
        if (startup_resume->is_live()) {
            if (!app->AttachSessionGroup(session_id).empty()) startup_resume = std::nullopt;
        }
        if (startup_resume) (void)app->ResumeTab(session_id);
    }
    // The focus seam's target, now that AppState exists (host_seams.hpp:
    // plugin load ran before app construction, so the provider attaches here).
    // Plugins captured the provider POINTER at registration; anything they
    // read through it happens later, so the attach is never raced.
    env.host_focus_provider->Attach(app.get());
    // Memory bounds (track R2): ui.json's values, or the built-in defaults
    // (SetUiBounds treats 0 as "keep the default"). Applied before any tab can
    // open, so every SessionState starts bounded.
    app->SetUiBounds(env.ui.scrollback_window_max_messages, env.ui.tool_output_buffer_bytes);
    // FULLSCREEN ALTERNATE SCREEN, not TerminalOutput: the alternate buffer is
    // repaint-safe on terminal resize. TerminalOutput repaints the PRIMARY
    // screen by cursor arithmetic over the previous frame, and a resize reflows
    // the text ftxui still thinks it owns -- every repaint then re-stamps stale
    // frame fragments (old tab-bar lines, typed text) as "junk" above the live
    // frame. Any out-of-band stdout/stderr write would desync it the same way.
    auto screen = ftxui::ScreenInteractive::FullscreenAlternateScreen();
    // ftxui 7 force-handles Ctrl+C by default -- it exits the loop and
    // re-raises SIGINT EVEN when a component consumes the event -- which
    // defeats this client's busy-cancel and quit confirmation. The client's
    // own dispatch (keybinding_dispatch.cpp) must see Ctrl+C first.
    screen.ForceHandleCtrlC(false);
    auto quit = screen.ExitLoopClosure();

    // --- frame state (loop thread only) --------------------------------------
    // Per-tab prompt editors (prompt_editor.hpp): one PromptEditorState per
    // tab -- a draft, its @-mention list and debounce are THAT tab's, and a
    // tab switch keeps the other tab's draft intact -- plus the ONE shared
    // recall timeline (up-arrow history is app-scoped by design). The host
    // component swaps the focused tab's editor in live, at render and event
    // time; the pre-tab "" entry serves the empty-app frame (typing there
    // submits through focused_or_new_tab, which opens a tab -- exactly the
    // old single-editor behavior).
    minimal::client::PerTabPromptEditors prompt_editors;
    minimal::client::PromptHistory prompt_history;
    // Up-arrow recall survives restarts (data dir, beside the client log).
    try {
        prompt_history.PersistTo(minimal::common::DataDir() / "prompt_history.json");
    } catch (const std::exception& error) {   // no $HOME: recall stays per-run
        minimal::common::DefaultLogger().warn("prompt history not persisted: {}", error.what());
    }
    // Declared here (not beside `suggest` below, which is its only other
    // reader) so close_tab -- defined before the rest of the editor wiring
    // exists -- can erase a closed tab's slot and not leak it.
    minimal::client::MentionQuerySlots mention_slots;
    bool show_panel = false;
    // Every modal's visibility flag lives in the stack (view/modal_stack.hpp);
    // these names are references into it, for the code below that opens and
    // closes them.
    minimal::client::ModalStack modals;
    using minimal::client::ModalId;
    bool& show_picker = modals.Flag(ModalId::kSessionPicker);
    bool& show_agent_picker = modals.Flag(ModalId::kAgentPicker);
    bool& show_model_picker = modals.Flag(ModalId::kModelPicker);
    bool& show_subagent_picker = modals.Flag(ModalId::kSubagentPicker);
    bool& show_theme_picker = modals.Flag(ModalId::kThemePicker);
    bool& show_keymap_help = modals.Flag(ModalId::kKeymapHelp);
    bool& show_commands_help = modals.Flag(ModalId::kCommandsHelp);
    bool& show_quit_confirm = modals.Flag(ModalId::kQuitConfirm);
    bool& show_permission = modals.Flag(ModalId::kPermission);
    bool& show_confirmation = modals.Flag(ModalId::kConfirmation);
    bool& show_delete_confirm = modals.Flag(ModalId::kDeleteConfirm);
    bool& show_plugin_modal = modals.Flag(ModalId::kPluginModal);
    bool& show_prompt = modals.Flag(ModalId::kPluginPrompt);
    std::string staged_delete_id;   // the picker delete confirmation's subject
    PendingClose pending_close;

    // Per-tab UI caches, keyed by TAB id (AppState's tab handle -- the only
    // key that exists for a sessionless tab). TabUi's type and the
    // maintenance logic that reads/writes it both live in
    // view/tab_ui.{hpp,cpp}; the MAP stays a plain RunTui local (not owned
    // by TabMaintenance) because close_tab (below) erases from it directly,
    // and close_tab is defined before TabMaintenance can be constructed --
    // it needs open_picker/plugin_modal_child, built later.
    std::map<std::string, minimal::client::TabUi> tab_ui;

    // Tab close, for every caller (keybinding, quit confirmation, quit,
    // /detach-session): the per-tab UI caches must die WITH the tab --
    // tab_ui entries were never erased, leaking each closed tab's streaming
    // buffer, grants and surface body for the process lifetime. Defined
    // HERE (before submit_command) because /detach-session intercepts the
    // command client-side and erases the group's tabs through it.
    auto close_tab = [&](const std::string& id) {
        app->CloseTab(id);
        tab_ui.erase(id);
        // The tab's editor dies WITH the tab (state AND component together --
        // the component's closures captured the state's address; neither may
        // outlive the other). The "" pre-tab entry is never forgotten.
        prompt_editors.ForgetTab(id);
        mention_slots.Forget(id);
    };

    // Picker-open lambdas, assigned after PickerController exists (below):
    // submit_command (above's definition point) intercepts the bare
    // /sessions //agent //model forms on sessionless tabs and needs to reach
    // these without a server round trip.
    std::function<void()> open_picker_fn = [] {};
    std::function<void()> open_agent_picker_fn = [] {};
    std::function<void()> open_model_picker_fn = [] {};
    std::function<void()> open_subagent_model_picker_fn = [] {};
    std::function<void()> open_theme_picker_fn = [] {};

    // Keymap-help overlay opener, assigned after the overlay exists (below):
    // submit_command intercepts bare /keymap the same way it intercepts bare
    // /help -- the overlay is where the full binding listing lives.
    std::function<void()> open_keymap_help_fn = [] {};

    // `focused`/`focused_details` become aliases into TabMaintenance once it
    // exists (below, after open_picker/plugin_modal_child) -- see the
    // TabMaintenance construction for why it cannot be built here yet.

    // --- editor ---------------------------------------------------------------
    //
    // T2: @-mention suggestions used to run SuggestPaths synchronously here,
    // on the loop thread, from inside the prompt editor's own Renderer --
    // a merely slow server stalled every keystroke after '@' for up to the
    // RPC's 250 ms deadline. The suggestor's contract (prompt_editor.hpp's
    // MentionSuggestor) is "nullopt = still waiting, poll again next frame";
    // the mechanics now live in MakeMentionSuggestor (prompt_editor.cpp) so
    // they are unit-testable: one in-flight slot PER TAB (MentionQuerySlots)
    // -- the single shared slot this lambda used before let a second tab's
    // query (or this tab re-firing on refocus) supersede the first, whose
    // answer the staleness guard then dropped -- "@ suggestions stopped
    // working", intermittently. Sessioned tabs query by session id (the
    // server resolves the root); a sessionless tab scopes to the TUI's cwd,
    // the root its first message would create the session with.
    auto suggest = minimal::client::MakeMentionSuggestor(
        app.get(), mention_slots, [](const std::string&) {
            return minimal::client::CurrentProjectRootOrEmpty();
        });

    // --- tabs are sessionless until the first message ---------------------------
    // OpenTab mints a TAB (a client-side container, no RPC, no server row);
    // the session comes into existence only when a message is submitted
    // (SubmitChatTurn -> AppState::EnsureSession, single-flight per tab on the
    // RPC worker). The old create_session_and_run queue-and-create machinery
    // lives inside AppState now, per tab; RunTui just opens-or-reuses a tab
    // and hands it the action. This is the structural fix for the 0-message
    // junk sessions: nothing reaches the server until there is something to
    // say, and a tab abandoned before that leaves no row at all.
    auto focused_or_new_tab = [app]() -> std::string {
        if (std::string tab = app->FocusedTabId(); !tab.empty()) return tab;
        return app->OpenTab(DefaultCreateOptions());   // local-only; focuses itself
    };
    auto submit_chat = [app, &tab_ui, focused_or_new_tab](const std::string& text) {
        const std::string tab = focused_or_new_tab();
        // Sending a turn returns the view to the newest row: scrolled-up
        // content would otherwise sit frozen while the reply streams in
        // somewhere below the visible viewport, reading as if nothing
        // happened. Same for a row cursor left over from browsing -- it would
        // otherwise keep pinning the view to a now-stale row through the
        // whole new turn. (A sessionless tab has no chat view yet -- the map
        // lookup misses -- and nothing to scroll.)
        if (auto it = tab_ui.find(tab); it != tab_ui.end() && it->second.chat) {
            it->second.chat->ScrollToBottom();
            it->second.chat->ClearRowFocus();
        }
        app->SubmitChatTurn(tab, text);
    };
    // --- theme switching -------------------------------------------------------
    // THE SWAP PATH, shared by "/theme <name>" and the picker's Enter: replace
    // `theme`'s CONTENTS in place (the object keeps its address -- every view,
    // the syntax highlighter and the markdown renderer hold references/pointers
    // to it and re-read ColorFor per frame, so the next paint is already the new
    // palette), drop the highlighter's color-baked cache, re-push OnTheme to the
    // theme-aware plugins, and record the choice in ui.json for the next launch.
    // A failed load changes nothing and reports the error through the tab
    // notice -- a typo'd name must not blank the UI.
    auto apply_theme = [app, &env, &theme](const std::string& name) {
        Theme loaded;
        try {
            loaded = Theme::Load(env.config_dir / "themes", name, env.depth, &env.loaded_plugins);
        } catch (const std::exception& error) {
            if (const std::string tab = app->FocusedTabId(); !tab.empty()) {
                app->TabNotice(tab, "theme: " + std::string(error.what()));
            }
            return;
        }
        theme = std::move(loaded);
        env.highlighter->OnThemeChanged();
        // PushThemeToPlugins disables a throwing chat surface only in `env`
        // (correct for the FIRST call, during BuildTuiEnvironment, before
        // AppState exists -- RunTui reads env.chat_surface into AppState
        // right after). This is a LATER call, after AppState already has its
        // own cached pointer and sticky broken-flag (EffectiveChatSurface) --
        // without this sync, a surface that throws on a SWITCH (not the
        // first load) stays the effective surface, still receiving events.
        const bool surface_was_live = env.chat_surface != nullptr;
        minimal::client::PushThemeToPlugins(env);
        if (surface_was_live && env.chat_surface == nullptr) {
            app->DisableChatSurface("threw on theme switch");
        }
        minimal::client::SaveThemePreference(env.config_dir, name);
        if (const std::string tab = app->FocusedTabId(); !tab.empty()) {
            app->TabNotice(tab, "theme switched to '" + name + "'");
        }
    };
    // The picker's row set: the stems of <config_dir>/themes/*.json. Sorted so
    // the menu is stable across opens; a missing/unreadable dir yields an empty
    // list (the picker's empty_message covers it -- same degrade rule as every
    // other decoration read).
    auto list_theme_names = [&env]() -> std::vector<std::string> {
        std::vector<std::string> names;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(env.config_dir / "themes",
                                                                     ec)) {
            std::error_code entry_ec;
            if (!entry.is_regular_file(entry_ec) || entry.path().extension() != ".json") {
                continue;
            }
            names.push_back(entry.path().stem().string());
        }
        std::sort(names.begin(), names.end());
        return names;
    };
    // THE CLIENT-SIDE /-COMMANDS (command_router.hpp): one table, listing and
    // behavior together. A command with a summary is the client's own and is
    // offered in /-autocomplete; one without is a server command whose `run`
    // intercepts particular forms and returns false for the rest.
    //
    // /rewind and /redo are client-native ClientEvents, not server Commands
    // (the client resolves "N user turns back" to a seq itself); /new,
    // /detach-session and /theme are client operations the server has no
    // part in; /pause and /resume send SetPausedEvent directly (a /-command
    // would queue for the worker and land only at a turn boundary -- the
    // delay pause exists to avoid). Bare /help, /keymap, /agent, /model,
    // /subagent-model and (sessionless) /sessions open client-owned overlays
    // with no round trip.
    const auto clear_focused_notice = [app] {
        if (const std::string tab = app->FocusedTabId(); !tab.empty()) app->ClearTabNotice(tab);
    };
    const auto focused_notice = [app](const std::string& text) {
        if (const std::string tab = app->FocusedTabId(); !tab.empty()) app->TabNotice(tab, text);
    };
    minimal::client::CommandRouter command_router;
    using minimal::client::ClientCommand;
    using minimal::client::CommandInvocationText;
    // "/help <command>" still goes to the server: one command's usage is a
    // short notice. The bare listing is the filterable overlay -- the notice
    // line's handful of rows could not hold it.
    command_router.Add({"help", "", true, false, [&](const CommandInvocationText& parsed) {
        if (!parsed.positional.empty()) return false;
        clear_focused_notice();   // an overlay sets no notice; drop a stale one
        show_commands_help = true;
        return true;
    }});
    // Arguments are accepted and ignored: there is no per-binding detail.
    command_router.Add({"keymap", "show all configured key bindings", true, false,
                        [&](const CommandInvocationText&) {
        clear_focused_notice();
        open_keymap_help_fn();
        return true;
    }});
    for (const bool pause : {true, false}) {
        command_router.Add({pause ? "pause" : "resume",
                            pause ? "pause the agent (stops after its current tool call)"
                                  : "resume a paused session (continues from its last tool "
                                    "result)",
                            false, true, [app, pause, focused_notice](const CommandInvocationText&) {
            // RequestPaused reads the focused TAB's snapshot and targets its
            // session; the confirming metadata event refreshes the status
            // bar. An explicit /resume with nothing queued kicks the seedless
            // continuation server-side (Session::SetPaused).
            const auto toggled = app->RequestPaused(pause);
            if (toggled == AppState::PauseToggle::NoSession) {
                focused_notice(std::string(pause ? "pause" : "resume") +
                               " needs a session; send a message to start one first");
            } else if (toggled == AppState::PauseToggle::AlreadyPaused ||
                       toggled == AppState::PauseToggle::AlreadyRunning) {
                focused_notice(pause ? "already paused" : "not paused");
            } else {
                focused_notice(pause ? "pausing -- the agent stops after its current tool call"
                                     : "resumed");
            }
            return true;
        }});
    }
    command_router.Add({"redo", "restore the most recently rewound turn", false, true,
                        [app, focused_notice](const CommandInvocationText&) {
        const std::string sid = app->FocusedSessionId();
        // Refused VISIBLY on a sessionless tab: SendRewind with no session
        // silently did nothing.
        if (sid.empty()) {
            focused_notice("/redo needs a session; send a message to start one first");
        } else {
            app->SendRewind(sid, std::nullopt, /*redo=*/true);
        }
        return true;
    }});
    command_router.Add({"rewind", "undo the most recent user turn (or N of them)", false, true,
                        [app, focused_notice](const CommandInvocationText& parsed) {
        const std::string sid = app->FocusedSessionId();
        if (sid.empty()) {
            focused_notice("/rewind needs a session; send a message to start one first");
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
                focused_notice("/rewind needs a positive number of turns, e.g. /rewind 2");
                return true;
            }
        }
        // nullopt covers both "fewer than n user turns in the loaded window"
        // and FindRewindTargetSeq's refusal on a trimmed window (its front is
        // no longer the true newest message).
        if (const auto seq = app->FindRewindTargetSeq(sid, n)) {
            app->SendRewind(sid, *seq, /*redo=*/false);
        } else {
            focused_notice("/rewind: could not find that many turns in the loaded history");
        }
        return true;
    }});
    // Always a NEW sessionless tab -- a sibling workspace, not a reuse of the
    // focused one. Nothing is created server-side until its first message.
    command_router.Add({"new", "start a new session", false, false,
                        [app](const CommandInvocationText&) {
        app->SetFocusedTab(app->OpenTab(DefaultCreateOptions()));
        return true;
    }});
    // A TAB operation: end the group's streams with EndSession{DETACH} and
    // erase the tabs (TabsToClose: the focused root plus its children, or
    // the whole group from a child). The sessions keep running server-side
    // and /sessions reattaches them. No confirmation: nothing is destroyed.
    command_router.Add({"detach-session",
                        "detach this tab group; work keeps running, /sessions reattaches",
                        false, true, [&, app](const CommandInvocationText&) {
        const std::string focused_tab = app->FocusedTabId();
        if (focused_tab.empty()) return true;
        if (app->FocusedSessionId().empty()) {
            close_tab(focused_tab);   // nothing to detach; closing is the whole job
            return true;
        }
        const auto ids = minimal::client::TabsToClose(app->GetAllTabs(), focused_tab);
        app->EndTabSessions(ids, minimal::client::EndSessionMode::kDetach);
        for (const auto& id : ids) close_tab(id);
        focused_notice("detached -- /sessions reattaches a live session");
        return true;
    }});
    // Decoration, not session state: works on any tab. Bare opens the
    // picker, named swaps directly (apply_theme).
    command_router.Add({"theme", "switch the UI theme", true, false,
                        [&](const CommandInvocationText& parsed) {
        if (parsed.positional.empty()) {
            clear_focused_notice();
            open_theme_picker_fn();
        } else {
            apply_theme(parsed.positional.front());
        }
        return true;
    }});
    // THE DESIRED-STATE COMMANDS, identical on every tab: bare opens the
    // client-owned picker; named sets the tab's desired state (riding
    // CreateSessionRequest on a sessionless tab, converging at the next turn
    // on a sessioned one) -- never a direct switch, so the status bar shows
    // the fresh value at once, pending until the metadata event confirms.
    for (const bool is_agent : {true, false}) {
        command_router.Add({is_agent ? "agent" : "model", "", false, false,
                            [&, is_agent](const CommandInvocationText& parsed) {
            if (parsed.positional.empty()) {
                clear_focused_notice();
                if (is_agent) {
                    open_agent_picker_fn();
                } else {
                    open_model_picker_fn();
                }
                return true;
            }
            const std::string& value = parsed.positional.front();
            const std::string tab = focused_or_new_tab();
            if (tab.empty()) return true;
            if (is_agent) {
                app->StageAgentPreset(tab, value);
            } else {
                // Shape-checked here (the parser the headless --model flag
                // uses) so a typo stages nothing; whether it RESOLVES is
                // still the server's call.
                if (!minimal::common::SplitModelRef(value)) {
                    app->TabNotice(tab, "'" + value +
                                            "' must be <provider>/<model>, e.g. local/qwen4 (or "
                                            "run /model with no arguments)");
                    return true;
                }
                app->StageModel(tab, value);
            }
            app->TabNotice(tab, std::string(is_agent ? "agent '" : "model '") + value +
                                    "' will be used on your next message");
            return true;
        }});
    }
    // NOT a desired-state command: its subject is a session's own setting.
    // Only the bare form on a sessioned tab is the client's (the picker);
    // everything else goes to the server, which validates the ref and
    // answers the sessionless refusal.
    command_router.Add({"subagent-model", "", false, true,
                        [&](const CommandInvocationText& parsed) {
        if (!parsed.positional.empty() || app->FocusedSessionId().empty()) return false;
        clear_focused_notice();
        open_subagent_model_picker_fn();
        return true;
    }});
    // Bare /sessions on a SESSIONLESS tab opens the picker outright: the
    // server's answer was only ever a view_hint pointing at it, and the
    // round trip minted a junk session. On a sessioned tab it goes to the
    // server like any other command.
    command_router.Add({"sessions", "", true, false, [&](const CommandInvocationText& parsed) {
        if (!parsed.positional.empty() || !app->FocusedSessionId().empty()) return false;
        open_picker_fn();
        return true;
    }});
    // Plugin-contributed client commands (ClientCommandRegistry), after the
    // client's own: a name the table already holds stays the client's. A
    // plugin command needing a session is refused visibly on a sessionless
    // tab, the same rule the server's catalog gets below.
    for (const auto& plugin_command : env.client_command_registry.commands()) {
        const std::string name(plugin_command->name());
        if (command_router.Has(name)) {
            minimal::common::DefaultLogger().warn(
                "client plugin command '/{}' is ignored: the client already has a command "
                "by that name",
                name);
            continue;
        }
        const bool needs_session = plugin_command->requires_session();
        command_router.Add({name, std::string(plugin_command->summary()), false, needs_session,
                            [&, command = plugin_command.get(), name,
                             needs_session](const CommandInvocationText& parsed) {
            if (needs_session && app->FocusedSessionId().empty()) {
                focused_notice("/" + name + " needs a session; send a message to start one first");
                return true;
            }
            try {
                command->Run(parsed.positional, parsed.raw);
            } catch (const std::exception& error) {
                focused_notice("/" + name + " failed: " + error.what());
            } catch (...) {
                focused_notice("/" + name + " failed");
            }
            return true;
        }});
    }
    // The catalog lists the table's own commands after every server list;
    // seeded only now that the table is complete.
    command_catalog_cache.SetClientSide(command_router.CatalogEntries());
    if (seed_catalog) {
        command_catalog_cache.Seed({}, minimal::client::CurrentProjectRootOrEmpty(),
                                   std::move(*seed_catalog));
    }

    // Everything the table does not handle. SESSIONLESS TABS: a command the
    // server catalog marks requires_session is refused client-side, visibly,
    // without creating a session; the genuinely sessionless ones ride
    // RunSessionlessCommand.
    auto submit_command = [&](const std::string& line) {
        if (command_router.Dispatch(line)) return;
        const auto parsed = minimal::client::SplitCommand(line);
        const bool has_session = !app->FocusedSessionId().empty();
        if (!has_session) {
            // Everything else the catalog marks requires_session is refused
            // here, visibly, without creating a session. A name ABSENT from
            // the catalog is reported as what it is -- unknown -- rather
            // than riding the session notice (a removed command reading as
            // "start a session first" was exactly the /providers
            // confusion). Only when the catalog actually arrived, though:
            // after a failed ListCommands it is empty and knows nothing.
            bool known = false;
            bool sessionless = false;
            const auto scope = command_scope();
            const bool commands_listed =
                command_catalog_cache.Listed(scope.session_id, scope.project_root);
            for (const auto& entry : *current_catalog()) {
                if (entry.name == parsed.name) {
                    known = true;
                    sessionless = !entry.requires_session;
                    break;
                }
            }
            if (const std::string tab = focused_or_new_tab(); !tab.empty()) {
                if (!known && commands_listed) {
                    app->TabNotice(tab, "unknown command: /" + parsed.name + " (try /help)");
                } else if (!sessionless) {
                    app->TabNotice(tab, "/" + parsed.name +
                                            " needs a session; send a message to start one first");
                } else {
                    app->RunSessionlessCommand(tab, line);
                }
            }
            return;
        }
        app->SubmitCommand(app->FocusedTabId(), line);
    };
    // Bracketed-paste flag, shared with HandleGlobalKey (keybinding_dispatch.cpp)
    // via KeyContext::pasting: a macOS Option-glyph byte pasted verbatim
    // (key_event_parser.cpp's IsOptionGlyphEvent) is byte-identical to the
    // key it would otherwise bind, so HandleGlobalKey needs to know a paste
    // is in progress to defer to the editor instead of firing the action.
    const auto pasting = std::make_shared<bool>(false);
    // The factory closure RunTui owned for the single editor, unchanged
    // except for taking the per-tab state as its argument. Invoked at most
    // once per tab (prompt_editor.cpp's Resolve), on the FTXUI loop thread,
    // so capturing RunTui locals by reference is as safe as it was before --
    // they all outlive the frame loop.
    auto build_prompt_editor = [&](minimal::client::PromptEditorState& state) {
        return minimal::client::MakePromptEditor(
            theme, state,
            submit_chat,
            submit_command,
        suggest,
        // The pull-model snapshot source (CommandCatalogCache): per frame
        // while a command name is typed, which is also what triggers the
        // background refresh.
        current_catalog,
        // The requires_session filter's probe (cheap: one locked read in
        // AppState). An empty FocusedSessionId means no tab, or a tab whose
        // session does not exist yet -- both hide the session-bound entries.
        // The subagent probe feeds the read-only gate (UsableOnSubagent):
        // a child's command surface is read-only server-side, so a subagent
        // tab is offered only what would not be refused there.
        [&app]() { return !app->FocusedSessionId().empty(); },
        [&app]() { return app->FocusedTabIsSubagent(); },
        pasting,
        // Recall is app-scoped: every tab's editor records into and recalls
        // from the ONE timeline (see prompt_editor.hpp's PromptHistory).
        &prompt_history,
        // "prompt.newline": lets a user rebind the newline-insert key away
        // from Alt+Enter (see prompt_editor.hpp's NEWLINE comment for why a
        // macOS user might need to -- Option+Enter is byte-ambiguous with
        // plain Enter on several terminals until configured).
        [&keymap](const ftxui::Event& event) { return keymap.Matches("prompt.newline", event); });
    };
    // The stable FTXUI tree node: resolves the FOCUSED tab's editor at
    // render AND event time (a key in the same frame as a tab switch lands
    // in the tab focused at event time -- the same rule the submit paths
    // follow via focused_or_new_tab).
    auto editor = minimal::client::MakePerTabPromptEditor(
        prompt_editors, [&app] { return app->FocusedTabId(); }, build_prompt_editor);

    // --- permission modal (inner swapped per outstanding request) -------------
    // The host is a STABLE component in the tree; its inner component is
    // rebuilt when a new request_id arrives. Checkbox state lives in
    // GrantToggles (keyed by request in tab_ui), so it survives the rebuild.
    // MakeModalHost binds show_permission by reference -- a by-value capture
    // latched `false` forever here once: the prompt rendered but no key ever
    // reached it (the "frozen UI" class). Same for the confirmation and
    // picker hosts below.
    auto permission_child = std::make_shared<Component>();
    auto permission_host_interactive =
        minimal::client::MakeModalHost(permission_child, show_permission);

    // The server confirmation modal (/clear asks through the
    // ConfirmationBroker, which BLOCKS the session worker until answered):
    // same host pattern as the permission prompt -- a STABLE component whose
    // inner modal is rebuilt when a new request_id arrives. The answer path
    // (AnswerConfirmation) existed and was dead code; nothing rendered the
    // request, so the tab wedged in Waiting until some unrelated event
    // arrived. Security-adjacent: it nests directly below the permission
    // prompt in the stack (see the modal stack below -- the two are mutually
    // exclusive in SessionState, so they never compete for the layer).
    auto confirmation_child = std::make_shared<Component>();
    auto confirmation_host_interactive =
        minimal::client::MakeModalHost(confirmation_child, show_confirmation);

    // The model's ask modal (tool-ask plugin): same stable-host pattern. A
    // question can only arrive while its session's worker is parked inside
    // the ask tool, so like the confirmation modal it nests below the
    // security prompts -- the three pending modal kinds are mutually
    // exclusive in SessionState.
    auto prompt_child = std::make_shared<Component>();
    auto prompt_host_interactive = minimal::client::MakeModalHost(prompt_child, show_prompt);

    // --- quit / close confirmation --------------------------------------------
    // THREE answers, not two (the close-vs-detach split): "close anyway"
    // destroys the sessions and everything in flight (EndSession{TERMINATE});
    // "detach" ends only the streams -- the sessions keep running
    // server-side, work finishes unattended, the server shuts itself down
    // once nothing is busy; "cancel" (esc) backs out.
    PendingConfirmation confirm_request;
    confirm_request.kind = "app_quit";
    confirm_request.title = "work is still running";
    confirm_request.detail =
        "a session still has work in flight. close: destroy it now. "
        "detach: keep it running in the background (/sessions reattaches).";
    confirm_request.options = {"close anyway", "cancel", "detach"};
    auto confirm_modal = minimal::client::MakeConfirmationModal(
        confirm_request, theme, [&](const std::string& chosen) {
            if (chosen != "close anyway" && chosen != "detach") {
                show_quit_confirm = false;
                pending_close = PendingClose{};
                return;
            }
            // State the intent on the wire FIRST, while the streams are
            // still open: DETACH or TERMINATE per the answer. Then close the
            // tabs root first, children after (the staging order -- see
            // PendingClose); each Close() lands on an already-ended stream
            // and degrades to a no-op.
            app->EndTabSessions(pending_close.tab_ids,
                                chosen == "detach" ? minimal::client::EndSessionMode::kDetach
                                                   : minimal::client::EndSessionMode::kTerminate);
            for (const auto& id : pending_close.tab_ids) close_tab(id);
            if (pending_close.quit) quit();
            show_quit_confirm = false;
            pending_close = PendingClose{};
        });

    // --- session picker (inner rebuilt on each open) ---------------------------
    auto picker_child = std::make_shared<Component>();
    auto picker_host_interactive = minimal::client::MakeModalHost(picker_child, show_picker);

    // --- agent preset picker ---------------------------------------------------
    auto agent_picker_child = std::make_shared<Component>();
    auto agent_picker_host_interactive =
        minimal::client::MakeModalHost(agent_picker_child, show_agent_picker);

    // --- model picker -----------------------------------------------------------
    auto model_picker_child = std::make_shared<Component>();
    auto model_picker_host_interactive =
        minimal::client::MakeModalHost(model_picker_child, show_model_picker);

    // --- subagent picker (the /subagents view_hint) -------------------------------
    auto subagent_picker_child = std::make_shared<Component>();
    auto subagent_picker_host_interactive =
        minimal::client::MakeModalHost(subagent_picker_child, show_subagent_picker);

    // --- theme picker ------------------------------------------------------------
    // Like the pickers above: a stable host whose inner component is rebuilt
    // on each open (open_theme_picker, below) -- themes are read from disk per
    // open, so a theme dropped into the themes dir shows up without a restart.
    auto theme_picker_child = std::make_shared<Component>();
    auto theme_picker_host_interactive =
        minimal::client::MakeModalHost(theme_picker_child, show_theme_picker);

    // T8: typing on an empty app already starts a session directly
    // (submit_chat, above); the picker's "+ new session" row is the same
    // action reached from inside the modal, with no text to send yet. The
    // tab it opens is SESSIONLESS -- nothing is minted server-side until the
    // first message (submit_chat -> SubmitChatTurn -> EnsureSession), so an
    // abandoned "+ new" tab leaves no row behind.
    auto open_new_session = [&] {
        show_picker = false;
        if (const auto tab = app->OpenTab(DefaultCreateOptions()); !tab.empty()) {
            app->SetFocusedTab(tab);
        }
    };

    // Owns picker_data/agent_picker_presets and the fetch/rebuild/open
    // flows privately; writes rebuilt components into picker_child/
    // agent_picker_child (declared above -- the modal slots RunTui's own
    // frame assembly and MakeModalHost calls read from, unchanged).
    minimal::client::PickerController picker_controller(
        minimal::client::PickerController::Dependencies{
            *client, *app, theme, picker_child, agent_picker_child, model_picker_child,
            subagent_picker_child, show_picker, show_agent_picker,
            show_model_picker, show_subagent_picker, staged_delete_id,
            show_delete_confirm, open_new_session,
            // The desired-state pickers stage onto this -- the focused tab,
            // or (the empty startup app, zero tabs) a fresh sessionless one,
            // so a /agent //model choice made before any tab exists lands on
            // the tab the first message will use.
            focused_or_new_tab});
    auto open_picker = [&] { picker_controller.OpenSessionPicker(); };
    auto open_agent_picker = [&] { picker_controller.OpenAgentPicker(); };
    auto open_model_picker = [&] { picker_controller.OpenModelPicker(/*refresh=*/false); };
    auto open_subagent_model_picker = [&] { picker_controller.OpenSubagentModelPicker(); };
    auto open_subagents_picker = [&picker_controller](const std::string& parent) {
        picker_controller.OpenSubagentsPicker(parent);
    };
    // Bind the submit_command intercepts (declared before, assigned now --
    // they run only on user input, long after this line).
    open_picker_fn = open_picker;
    open_agent_picker_fn = open_agent_picker;
    open_model_picker_fn = open_model_picker;
    open_subagent_model_picker_fn = open_subagent_model_picker;
    auto open_theme_picker = [&] {
        *theme_picker_child = minimal::client::MakeThemePicker(
            list_theme_names(), theme.name(), theme,
            minimal::client::ThemePickerCallbacks{
                [&](const std::string& name) {
                    show_theme_picker = false;
                    apply_theme(name);
                },
                [&] { show_theme_picker = false; }});
        show_theme_picker = true;
    };
    open_theme_picker_fn = open_theme_picker;
    // Reset slots for the two help overlays: they are built once, so without
    // these the typed filter survived across opens -- reopening the help
    // presented the previous needle as if it were a fresh search.
    std::function<void()> keymap_help_reset;
    std::function<void()> commands_help_reset;
    auto open_keymap_help = [&] {
        if (keymap_help_reset) keymap_help_reset();
        show_keymap_help = true;
    };
    open_keymap_help_fn = open_keymap_help;

    // Built once, unlike the pickers above: env.keybindings is already final
    // by the time RunTui runs (BuildTuiEnvironment folds in the user's file
    // and every plugin view's EnsureDefault before returning), so this needs
    // none of PickerController's async-rebind machinery -- same reasoning as
    // confirm_modal/delete_modal below.
    auto keymap_help_view = minimal::client::MakeKeymapHelpView(
        env.keybindings.all(), theme, [&] { show_keymap_help = false; },
        &keymap_help_reset);

    // Built once, like keymap_help_view; the rows are rebuilt from
    // current_catalog on every open, so it lists the current snapshot. `theme`
    // is a RunTui local that outlives the frame. Opened by the bare `/help` interception
    // in submit_command -- the notice line's handful of rows could not hold
    // the full command listing.
    auto commands_help_view = minimal::client::MakeCommandsHelpView(
        current_catalog, theme, [&] { show_commands_help = false; },
        // Same requires_session exclusion the autocomplete applies: while the
        // focused tab has no session, the overlay lists only sessionless
        // commands (the overlay rebuilds its rows on every open, so the probe
        // is fresh each time). The subagent probe likewise: on a child tab,
        // only read-only commands are listed.
        [&app]() { return !app->FocusedSessionId().empty(); },
        [&app]() { return app->FocusedTabIsSubagent(); },
        &commands_help_reset,
        // Enter inserts "/name " with the caret after it, ready for args.
        // The overlay is app-wide; the insert lands in whatever tab is
        // focused when the user picks a command (CurrentState("" on an
        // empty app = the pre-tab editor, the same resolution the host
        // itself uses).
        [&prompt_editors, &app](const std::string& name) {
            minimal::client::PromptEditorState& state =
                prompt_editors.CurrentState(app->FocusedTabId());
            state.content = "/" + name + " ";
            state.cursor_position = static_cast<int>(state.content.size());
        });

    // --- plugin views (panel region + modal layer) -----------------------------
    // Panels: rendered in the frame beside the tools panel; one shared
    // CatchEvent (over the frame) routes events to visible ones. Modals: one
    // host in the modal stack BELOW the quit confirmation -- a plugin modal
    // can never preempt the security UI, which is the invariant the nesting
    // itself enforces (see the modal stack below).
    auto plugin_modal_child = std::make_shared<Component>();
    // NOT MakeModalHost: a throwing PLUGIN modal must hide itself for the
    // frame (never fatal), so the render side keeps its own containment.
    auto plugin_modal_host = Renderer([=] {
        if (!*plugin_modal_child) return emptyElement();
        try {
            return (*plugin_modal_child)->Render();
        } catch (const std::exception& error) {
            minimal::common::DefaultLogger().error("plugin modal threw: {}", error.what());
        } catch (...) {
            minimal::common::DefaultLogger().error("plugin modal threw a non-standard exception");
        }
        return emptyElement();
    });
    auto plugin_modal_host_interactive = CatchEvent(plugin_modal_host, [&](const Event& event) {
        if (!show_plugin_modal || !*plugin_modal_child) return false;
        // Host-level close, ahead of the plugin: FTXUI's Modal() gives this
        // component exclusive input while show_plugin_modal is true, so
        // HandleGlobalKey's own toggle keybinding -- "main" in Modal()'s
        // terms -- can never run again to close it (see
        // ClosePluginModal's header comment). A plugin whose own component
        // neither consumes Escape nor otherwise clears the flag would
        // otherwise trap every key until the terminal is killed.
        if (event == Event::Escape) {
            minimal::client::ClosePluginModal(plugin_views, plugin_modal_child, show_plugin_modal);
            return true;
        }
        // Plugin component code: a throw escaping into the ftxui loop would
        // std::terminate the process. Contained like the render side above --
        // logged, and the event treated as consumed.
        try {
            return (*plugin_modal_child)->OnEvent(event);
        } catch (const std::exception& error) {
            minimal::common::DefaultLogger().warn("plugin modal view threw on event: {}",
                                                  error.what());
        } catch (...) {
            minimal::common::DefaultLogger().warn(
                "plugin modal view threw on event (non-standard exception)");
        }
        return true;
    });

    // THE MAINTENANCE PASS: everything the frame needs but must not do from
    // Render -- a fresh focused-tab snapshot, the per-tab streaming/refresh
    // bookkeeping, and the once-per-notice VIEW side-effects (a View-display
    // notice may stage a modal and run a bounded ListSessions RPC; doing
    // that inside the renderer made the frame stall and the staged modal
    // unclosable). Constructed here, not earlier: it needs open_picker/
    // open_agent_picker (PickerController, above) and plugin_modal_child/
    // show_plugin_modal (above). `tab_maintenance` itself must never be
    // moved/copied for the rest of RunTui's lifetime -- Focused()/
    // FocusedDetails() return references into its own members.
    minimal::client::TabMaintenance tab_maintenance(minimal::client::TabMaintenance::Dependencies{
        *app, engine, view_registry, plugin_views, plugin_modal_child,
        show_plugin_modal, open_picker, open_agent_picker, open_model_picker,
        open_subagent_model_picker, open_subagents_picker,
        [&picker_controller] { picker_controller.Poll(); }, tab_ui, theme,
        message_registry, renderer_registry, display_registry, env.prompt_registry,
        permission_child, show_permission,
        confirmation_child, show_confirmation, prompt_child, show_prompt,
        // The chat views this builds name these keys in their own text; the
        // user's keymap decides them.
        env.keybindings.KeyFor("chat.toggle_collapse", "alt+c"),
        env.keybindings.KeyFor("chat.load_older", "pageup"),
        // ui.json's startup fold defaults (kFoldDefaultAuto when unset).
        env.ui.fold_thinking,
        env.ui.fold_tools,
        // Click-to-cancel for QUEUED message rows: resolve the tab's session
        // at click time (a tab's ChatView may predate its session).
        [app](const std::string& tab_id, std::uint64_t turn_id) {
            app->SendCancelQueuedTurnByTab(tab_id, turn_id);
        }});
    // Same names the inline code declared directly -- every lambda below
    // this point (with_keys' KeyContext, the still-inline render body)
    // captures `[&]`, i.e. by ordinary lexical scoping.
    const std::string& focused = tab_maintenance.Focused();
    const SessionState::Snapshot& focused_details = tab_maintenance.FocusedDetails();

    // --- session picker delete confirmation ------------------------------------
    // Built once, like the quit confirmation; `delete_request` is mutated when
    // a delete is staged (single-threaded loop, so the write is safe).
    PendingConfirmation delete_request;
    delete_request.kind = "session_delete";
    delete_request.title = "delete session?";
    delete_request.detail = "the transcript and its subagent sessions are removed permanently";
    delete_request.options = {"delete", "cancel"};
    auto delete_modal = minimal::client::MakeConfirmationModal(
        delete_request, theme, [&](const std::string& chosen) {
            const bool confirmed = chosen == "delete";
            const std::string id = staged_delete_id;
            if (confirmed && !id.empty()) {
                // Off the loop thread: DeleteSession carries a 2 s deadline
                // (session_client.cpp), and this callback fires from inside
                // the event loop -- a slow server used to stall the whole
                // frame for the duration. `client` is captured by shared_ptr
                // (not `&`): the job may still be in AppState's queue when
                // this scope ends, and app->StopRpcQueue() (below, before
                // teardown) joins the worker before `client` itself would go
                // away, so the copy just needs to survive until then. NOT
                // stderr on failure: the alternate screen is live (see
                // ScreenInteractive::FullscreenAlternateScreen() below), and
                // an out-of-band stdout/stderr write from the worker thread
                // would desync the repaint the same way a resize does, and
                // the message was invisible behind the fullscreen buffer
                // regardless -- logged instead, same as every other
                // background-job failure in this file.
                app->RunOnRpcQueue([client, id] {
                    if (const auto status = client->DeleteSession(id); !status.ok()) {
                        minimal::common::DefaultLogger().error("delete failed: {}",
                                                               status.error_message());
                    }
                }, /*must_run=*/true, AppState::RpcLane::kBulk);
            }
            staged_delete_id.clear();
            show_delete_confirm = false;
            // The confirm is only ever staged FROM the session picker (which
            // it hides for its duration -- it sits below the picker in the
            // modal stack above), so resolving it returns there. Cancel:
            // nothing changed, so the picker's slot component is untouched
            // and its filter/cursor/sort survive as the user left them.
            // Confirmed delete: re-fetch so the removed row drops out of the
            // list (that rebuild starts a fresh filter and cursor -- the
            // same tradeoff the picker's ctrl+a toggle already accepts).
            show_picker = true;
            if (confirmed && !id.empty()) {
                picker_controller.RefreshSessionPicker();
            }
        });

    // --- first run --------------------------------------------------------------
    // T8 used to create a session here unconditionally, whether or not prior
    // sessions existed for the cwd -- avoiding a picker-first modal plus TWO
    // ListSessions round trips (a limit=1 probe here, then the picker's own
    // limit=200 fetch) for the common "start working" case. That traded a
    // startup CreateSession call every launch for a real cost: picking an
    // EXISTING session from ctrl+o left the just-created empty one open as a
    // second, junk tab (and a junk row in the sessions table). Startup creates
    // NOTHING -- and, since tabs became sessionless containers, neither does
    // anything else until the first message: every create path (typing, /new,
    // the picker's "+ new session") now opens a TAB only, and EnsureSession
    // mints the session when the user actually submits (see the sessionless-
    // tabs block above submit_chat). A command run on a sessionless tab never
    // mints one at all: sessionless commands ride RunSessionlessCommand,
    // session-bound ones are refused client-side, and the picker-opening bare
    // forms open their pickers directly.

    // --- frame -----------------------------------------------------------------
    const int panel_height = env.ui.panel_height > 0 ? env.ui.panel_height : 12;   // ui.json
    // AnyWorkInFlight (keybinding_dispatch.hpp): the caller's tab snapshots
    // carry the state already -- re-snapshotting every tab (a full deep copy)
    // per keypress was pure waste.

    // Quit = close every tab, with ONE confirmation if any work is in flight
    // (ARCHITECTURE.md "Session lifetime"). Shared by ctrl+q, tab-close's
    // sibling, and ctrl+c-when-idle (raw mode means ctrl+c reaches the loop
    // as a key event, not as SIGINT -- without this it was simply dead).
    auto request_quit = [&] {
        const auto tabs = app->GetAllTabs();
        std::vector<std::string> ids;
        for (const auto& tab : tabs) ids.push_back(tab.tab_id);
        // HasQueuedWork: a sessionless tab's EnsureSession job can still be
        // QUEUED (not yet started, so SessionStateKind hasn't moved off Idle
        // -- AnyWorkInFlight can't see it) when quit is pressed. StopRpcQueue
        // DISCARDS the queue rather than draining it, so without this check
        // a message typed moments before quit was silently dropped.
        if (!AnyWorkInFlight(tabs, ids) && !app->HasQueuedWork()) {
            quit();
        } else {
            pending_close = PendingClose{};
            pending_close.quit = true;
            pending_close.tab_ids = std::move(ids);
            show_quit_confirm = true;
        }
    };

    // FrameRenderer owns the tab body / status-bar / plugin-panel assembly;
    // constructed once, referencing RunTui's locals the same way
    // TabMaintenance/PickerController do. `frame_renderer` itself must never
    // be moved/copied for the rest of RunTui's lifetime (its Dependencies
    // hold references into RunTui's own objects, resolved once at
    // construction). The permission/confirmation modal rebuild lives in
    // TabMaintenance now (Part 3.2) -- FrameRenderer no longer touches
    // permission_child/show_permission/confirmation_child/show_confirmation.
    minimal::client::FrameRenderer frame_renderer(minimal::client::FrameRenderer::Dependencies{
        *app, theme, focused, focused_details, tab_ui, start_view_registry, message_registry,
        renderer_registry, display_registry, status_registry, engine, show_panel, panel_height,
        env.ui.tools_panel_stdout_buffer_lines > 0 ? env.ui.tools_panel_stdout_buffer_lines : 100,
        editor, status_format, plugin_views,
        // Row-renderer collapse hints follow the user's keymap, not a
        // hardcoded alt+c -- and so do the other key names the frame spells
        // out.
        env.keybindings.KeyFor("chat.toggle_collapse", "alt+c"),
        env.keybindings.KeyFor("session.picker", "ctrl+o"),
        env.keybindings.KeyFor("chat.load_older", "pageup"),
        env.keybindings.KeyFor("chat.scroll_bottom", "pagedown"),
        [&tab_maintenance](const std::string& tab_id, minimal::client::TabUi& ui) {
            tab_maintenance.EnsureChatView(tab_id, ui);
        }});
    auto session_view = Renderer(editor, [&] { return frame_renderer.Render(); });

    // --- global keybindings -----------------------------------------------------
    // The dispatch body itself lives in keybinding_dispatch.cpp
    // (HandleGlobalKey) -- constructed ONCE, not per-event: every field is
    // either a reference into a RunTui local that lives for the whole frame
    // loop (so re-deriving it per event would be pure waste, the same
    // argument keymap's own extraction already made) or a std::function
    // capturing those same locals by reference, cheap to build once and
    // reuse. `key_ctx` itself is captured by reference into the CatchEvent
    // lambda below, never copied.
    minimal::client::KeyContext key_ctx{
        *app, keymap, focused,
        show_panel, show_plugin_modal,
        plugin_views, plugin_modal_child,
        open_picker, open_agent_picker, open_model_picker, open_keymap_help, request_quit, close_tab,
        // ChatView::ScrollBy, if the tab has one yet (ui.chat is built
        // lazily inside session_view's own render lambda, so it may not
        // exist on the very first frame of a freshly opened tab).
        [&](const std::string& tab_id, int delta_rows) {
            WithChat(tab_ui, tab_id, [&](auto& chat) { chat.ScrollBy(delta_rows); });
        },
        // The wheel's own path: the gate in ScrollWheelBy bounds how fast a
        // momentum flick can walk the transcript (keyboard scrolling above
        // stays ungated).
        [&](const std::string& tab_id, int delta_rows) {
            WithChat(tab_ui, tab_id, [&](auto& chat) { chat.ScrollWheelBy(delta_rows); });
        },
        [&](const std::string& tab_id) {
            WithChat(tab_ui, tab_id, [](auto& chat) {
                chat.ScrollToBottom();
                chat.ClearRowFocus();
            });
        },
        // tab.close with work in flight: stage ONE confirmation for the
        // whole closing group, same as request_quit's own busy branch.
        [&](std::vector<std::string> ids) {
            pending_close = PendingClose{};
            pending_close.tab_ids = std::move(ids);
            show_quit_confirm = true;
        },
        [&](const std::string& tab_id, int delta) {
            WithChat(tab_ui, tab_id, [&](auto& chat) { chat.MoveRowFocus(delta); });
        },
        [&](const std::string& tab_id) {
            WithChat(tab_ui, tab_id, [](auto& chat) { chat.ToggleCollapseFocused(); });
        },
        [&](const std::string& tab_id) {
            WithChat(tab_ui, tab_id, [](auto& chat) { chat.ToggleCollapseAll(); });
        },
        // Mouse-click collapse: ToggleCollapseAt consumes the click only
        // when it landed on a collapsible row; a miss falls through.
        [&](const std::string& tab_id, int screen_y) -> bool {
            return WithChat(tab_ui, tab_id,
                            [&](auto& chat) { return chat.ToggleCollapseAt(screen_y); });
        },
        // Mouse-click cancel of a QUEUED message: CancelPendingAt consumes
        // the click only when a cancellable queued row is under it; a miss
        // falls through to the collapse toggle above's handling order.
        [&](const std::string& tab_id, int screen_y) -> bool {
            return WithChat(tab_ui, tab_id,
                            [&](auto& chat) { return chat.CancelPendingAt(screen_y); });
        },
        [&](const std::string& tab_id) {
            WithChat(tab_ui, tab_id, [](auto& chat) { chat.ToggleTimestamps(); });
        },
        // The fold toggles are session-wide preferences, unlike the
        // per-tab actions above: EVERY open tab's ChatView flips, and the
        // new state persists to ui.json (SaveFoldDefaults) so the next
        // launch starts there. Tabs opened AFTER a toggle start from it too:
        // TabMaintenance::SetFoldDefaults re-seeds the builder every new
        // ChatView comes from. The toggles return the same value from every
        // view, so one write.
        [&](const std::string& tab_id) {
            std::optional<int> next;
            for (auto& [tab, ui] : tab_ui) {
                if (ui.chat) next = ui.chat->ToggleThinkingFold();
            }
            if (next.has_value()) {
                // Same feedback shape /theme uses: a transient tab notice,
                // not a status-bar segment -- the rows just flipped visibly;
                // the notice names the state and whether it persisted.
                const bool saved =
                    minimal::client::SaveFoldDefaults(env.config_dir, next, std::nullopt);
                tab_maintenance.SetFoldDefaults(next, std::nullopt);   // tabs opened later
                app->TabNotice(tab_id,
                               std::string("thinking rows: ") +
                                   (*next == kFoldCollapsed ? "collapsed" : "expanded") +
                                   (saved ? " (saved)" : " -- ui.json NOT updated"));
            }
        },
        [&](const std::string& tab_id) {
            std::optional<int> next;
            for (auto& [tab, ui] : tab_ui) {
                if (ui.chat) next = ui.chat->ToggleToolFold();
            }
            if (next.has_value()) {
                const bool saved =
                    minimal::client::SaveFoldDefaults(env.config_dir, std::nullopt, next);
                tab_maintenance.SetFoldDefaults(std::nullopt, next);   // tabs opened later
                app->TabNotice(tab_id,
                               std::string("tool rows: ") +
                                   (*next == kFoldCollapsed ? "collapsed" : "expanded") +
                                   (saved ? " (saved)" : " -- ui.json NOT updated"));
            }
        },
        *pasting,
        [&prompt_editors, &app] { return prompt_editors.CurrentState(app->FocusedTabId()).list_open; },
        [&tab_ui](const std::string& tab_id, int y) {
            return WithChat(tab_ui, tab_id, [y](auto& chat) { return chat.ViewportContainsY(y); });
        },
        [&frame_renderer](int x, int y) { return frame_renderer.TabAt(x, y); }};
    auto with_keys = CatchEvent(session_view, [&](const Event& event) {
        return minimal::client::HandleGlobalKey(key_ctx, event);
    });

    // The modal stack (view/modal_stack.hpp owns the order: the security
    // prompts topmost, plugin modals lowest). Rooted at with_keys, NOT raw
    // session_view: E5's refactor once re-rooted it at session_view and
    // silently dropped every global keybinding.
    modals.Set(ModalId::kPluginModal, plugin_modal_host_interactive);
    modals.Set(ModalId::kDeleteConfirm, delete_modal);
    modals.Set(ModalId::kSessionPicker, picker_host_interactive);
    modals.Set(ModalId::kAgentPicker, agent_picker_host_interactive);
    modals.Set(ModalId::kModelPicker, model_picker_host_interactive);
    modals.Set(ModalId::kSubagentPicker, subagent_picker_host_interactive);
    modals.Set(ModalId::kThemePicker, theme_picker_host_interactive);
    modals.Set(ModalId::kKeymapHelp, keymap_help_view);
    modals.Set(ModalId::kCommandsHelp, commands_help_view);
    modals.Set(ModalId::kQuitConfirm, confirm_modal);
    modals.Set(ModalId::kPluginPrompt, prompt_host_interactive);
    modals.Set(ModalId::kConfirmation, confirmation_host_interactive);
    modals.Set(ModalId::kPermission, permission_host_interactive);
    const Component with_modals = modals.Build(with_keys);

    // The maintenance pass rides the event path ABOVE the whole stack: this
    // handler sees every event the loop processes (including the pump's
    // Event::Custom, even while a modal would consume ordinary keys), runs
    // the maintenance pass, and consumes NOTHING -- a pure pre-render
    // observation point. This is what keeps side-effects out of Render().
    // Coalescing for the pump's Event::Custom posts, cleared by the frame's
    // CatchEvent below (the one handler that sees every consumed event): a
    // frame render that cannot keep pace must not find a fresh post queued
    // every 50ms tick -- each queued Custom renders one FULL frame, so the
    // posts only lengthened a post-stall backlog instead of catching up.
    std::atomic<bool> custom_event_in_flight{false};
    Component final_tree = CatchEvent(with_modals, [&](const Event&) {
        // This event is consumed and its frame is about to render -- the
        // pump may queue its next post. Set here (not at post time) so at
        // most one un-consumed Custom is ever outstanding.
        custom_event_in_flight.store(false, std::memory_order_release);
        tab_maintenance.Run();
        // A paste can only be IN PROGRESS while the editor is the stack's
        // active leaf; any modal grabbing exclusive routing (FTXUI Modal()'s
        // own contract) swallows the rest of the paste, including its own
        // end marker, before the editor's CatchEvent (prompt_editor.cpp)
        // ever sees it to clear this itself. Self-heal here, the one place
        // that watches every event above the whole stack: the first event
        // after a modal opens mid-paste clears the flag, rather than
        // leaving it stuck true (every later Return silently inserting a
        // newline instead of submitting). tab_maintenance.Run() above
        // already applied this tick's snapshot, so a permission/
        // confirmation/ask modal that just became due is reflected in its
        // show_* flag by the time this check runs.
        if (*pasting && modals.AnyOpen()) {
            *pasting = false;
        }
        return false;
    });
    // The theme's optional canvas color, outermost so it reaches every modal
    // too. Read per frame: /theme swaps `theme` in place. A theme without
    // `background` leaves the terminal's own (possibly transparent) bg.
    final_tree = Renderer(final_tree, [inner = final_tree, &theme] {
        return inner->Render() | minimal::client::FillBackground(theme.ColorFor("background"));
    });
    tab_maintenance.Run();   // the first frame must not render an empty snapshot

    // Shutdown signal shared by both background threads below: notified once,
    // right after running.store(false), so a thread parked in a cv wait wakes
    // IMMEDIATELY instead of riding out its own poll interval. Only the
    // heartbeat actually waits on it (the pump still must wake every 50ms
    // regardless, to notice a UiVersion() change with no other trigger) --
    // shared rather than duplicated because both threads shut down at the
    // same instant for the same reason.
    std::mutex shutdown_mtx;
    std::condition_variable shutdown_cv;

    // THE PUMP: reader threads wake nothing by themselves; this loop redraws
    // at up to 20 Hz from snapshots. Redraws are gated on the app's UI
    // revision -- an idle TUI used to repaint the full frame 20x per second
    // no matter what. The 50ms wake itself is not optional (nothing else can
    // notice a UiVersion() change or the shutdown flag), but the OLD
    // unconditional "force a repaint every 4th tick regardless" floor is:
    // that existed only so the editor's @-mention debounce gate -- which
    // fires FROM a render pass (prompt_editor.cpp has no timer of its own) --
    // kept getting serviced while the user typed, and paid a full render
    // pass 5x/s for the whole app's life to cover a window that is usually
    // closed. Gated instead on query_pending_hint: a debounced query is
    // actually armed only for the ~100-150ms between a keystroke and its
    // answer, so this floor is now active for a sliver of the app's
    // lifetime rather than always. StreamingMarkdownView::PendingReparseHint
    // is the same seam for the streaming markdown debounce: DispatchToTab
    // bumps UiVersion() once per wire event (state.cpp), so a burst of
    // deltas that PAUSES wakes this pump once for the pause's last delta --
    // if that render landed before the debounce window closed, nothing
    // bumps the version again when it DOES close, and the tail sat
    // rendered-stale until an unrelated event happened to arrive.
    std::atomic<bool> running{true};
    std::thread pump([&] {
        uint64_t last_version = app->UiVersion();
        // Running-tool forced redraws are bucketed to whatever FormatDuration
        // can actually display (tool_status.hpp) instead of firing every
        // 50ms tick: a repaint pass is a full FTXUI tree walk + full-screen
        // terminal write (no dirty-tracking in this FTXUI version), so
        // forcing it 20x/s for the whole life of a long tool call was paying
        // real CPU (~1% idle -> ~4-5% while running) for ticks the display
        // couldn't even show. Sentinel far in the past so the very first
        // running tick fires immediately.
        auto last_running_redraw = std::chrono::steady_clock::now() - std::chrono::hours(1);
        while (running.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            const uint64_t version = app->UiVersion();
            // FocusedTabFastestRunningToolElapsedMs joins the same "force a
            // repaint with no version bump" seam: a foreground tool that goes
            // quiet (a long `sleep`, a silent build step) produces no wire
            // events at all, so nothing bumps UiVersion -- without this, both
            // the spinner and a live elapsed counter would freeze the moment
            // the tool stopped printing, reading as hung rather than merely
            // slow. Bucketed by elapsed time to match FormatDuration's own
            // resolution: <1s shows whole ms (needs the full 50ms cadence),
            // 1s-60s shows tenths of a second (100ms is enough), >=60s shows
            // whole seconds (1000ms is enough) -- so the redraw rate never
            // outruns what the number on screen can change to.
            bool running_tool_redraw_due = false;
            if (const auto elapsed = app->FocusedTabFastestRunningToolElapsedMs()) {
                const auto interval = *elapsed < std::chrono::milliseconds(1000)
                                           ? std::chrono::milliseconds(50)
                                       : *elapsed < std::chrono::milliseconds(60000)
                                           ? std::chrono::milliseconds(100)
                                           : std::chrono::milliseconds(1000);
                const auto now_steady = std::chrono::steady_clock::now();
                if (now_steady - last_running_redraw >= interval) {
                    running_tool_redraw_due = true;
                    last_running_redraw = now_steady;
                }
            }
            const bool debounce_pending =
                prompt_editors.QueryPendingHint(app->FocusedTabId()) ||
                minimal::client::StreamingMarkdownView::PendingReparseHint().load(
                    std::memory_order_acquire) ||
                running_tool_redraw_due || tab_maintenance.RefreshRetryDue();
            if (version != last_version || debounce_pending) {
                // Coalesce: skip the post while the previous Custom is still
                // unconsumed. last_version moves ONLY on a successful post,
                // so a skipped tick re-posts (with the newer version) on the
                // next one -- worst case one pump tick of extra latency,
                // never a stale frame.
                if (!custom_event_in_flight.exchange(true, std::memory_order_acq_rel)) {
                    last_version = version;
                    screen.PostEvent(Event::Custom);
                }
            }
        }
    });

    // THE HEARTBEAT: the server's idle exit (server.idle_shutdown_seconds,
    // default 5s) fires once zero sessions are live AND zero streams are
    // attached AND no recent Ping arrived. A TUI on its empty-state welcome
    // view -- before its first session, which is created lazily on the first
    // submission -- holds neither, so without a beat the server shut itself
    // down while the user was still composing that first message: the
    // reported "start client, fiddle with /-autocomplete, submit, server
    // stops". One Ping per ~2s marks the client alive; quitting the TUI
    // stops the beats and the server exits on its own linger as designed.
    //
    // A condition_variable wait, not a 100ms-poll/20-tick counter: the old
    // shape woke this thread 20x for every Ping it actually sent, and the
    // shutdown flag was only ever checked between those 100ms slices, so
    // quitting could wait up to 100ms on a thread doing nothing but counting.
    // wait_for's predicate returns true (ending the wait early) the instant
    // shutdown_cv is notified.
    std::thread heartbeat([&] {
        std::unique_lock<std::mutex> lock(shutdown_mtx);
        while (!shutdown_cv.wait_for(lock, std::chrono::seconds(2), [&] {
            return !running.load(std::memory_order_acquire);
        })) {
            (void)client->Ping();   // failures ignored -- see SessionClient::Ping
        }
    });

    // Bracketed paste (DECSET 2004): enabled for the duration of the loop, the
    // terminal wraps a paste in ESC[200~ ... ESC[201~ markers. ftxui 7 has no
    // paste support, but it forwards the markers verbatim as Event::Special
    // (unknown CSI -> Special, full bytes preserved), which the prompt editor
    // consumes to treat a multi-line paste as ONE turn instead of one
    // submission per line. RAII, not a post-Loop write: an exception escaping
    // the loop must not leave the terminal in paste mode.
    struct BracketedPasteGuard {
        ~BracketedPasteGuard() {
            std::fputs("\x1b[?2004l", stdout);
            std::fflush(stdout);
        }
    } bracketed_paste_guard;
    std::fputs("\x1b[?2004h", stdout);
    std::fflush(stdout);

    // Everything screen.Loop() needs undone before app/env/client unwind, in
    // the order their own comments require -- called once on the normal
    // return path below, and again from the catch block if a key handler (or
    // anything else running under the loop) throws. Without this, an
    // escaping exception skips straight to ~app/~env/~client: pump/heartbeat
    // are still joinable std::thread locals at that point, and a joinable
    // thread's destructor calls std::terminate() immediately -- before any of
    // the ordering below (which exists to avoid a deadlock or a
    // dereference-after-free) ever runs.
    auto shutdown_tui = [&] {
        {
            // Under shutdown_mtx, not a bare store: the heartbeat's
            // wait_for(lock, 2s, predicate) checks the predicate WHILE holding
            // this same lock. Without it, a store+notify landing exactly as the
            // heartbeat finishes checking (still true) but before it actually
            // blocks on the cv can be missed entirely -- bounded by the 2s
            // timeout floor, so not a hang, but a real extra 2s of quit delay
            // for no reason. Taking the lock here guarantees one of two
            // outcomes: the heartbeat is already blocked (wakes immediately), or
            // it hasn't checked yet (acquires the lock after this and sees
            // false right away).
            std::lock_guard<std::mutex> lk(shutdown_mtx);
            running.store(false, std::memory_order_release);
        }
        shutdown_cv.notify_all();   // wakes the heartbeat immediately, not on its next 2s tick
        pump.join();
        heartbeat.join();

        // Join the async RPC worker BEFORE tearing down tabs: a job's completion
        // (RequestScrollback et al.) publishes into SessionState/AppState the
        // same way a reader thread's dispatch does, and DetachAllTabs() below
        // destroys the very tabs a straggling job would publish into.
        //
        // The terminal is already restored by this point (screen.Loop() above
        // has returned or thrown -- either way it is off the stack), and
        // StopRpcQueue()'s join is unbounded against whatever job is currently
        // running (a blocking RPC with its own multi-second deadline --
        // EnsureSession up to 5s, a model-list refresh up to 15s). Without this
        // notice the process just sits there silently for up to that long,
        // looking hung rather than merely finishing up.
        if (app->RpcQueueBusy()) {
            std::fprintf(stderr, "minimal: waiting for a pending request to finish...\n");
        }
        app->StopRpcQueue();

        // S5.4: quitting the TUI DETACHES every remaining tab rather than
        // terminating it -- a session outlives the client that opened it, so
        // closing the terminal must not destroy in-flight work. With the new
        // idle-exit rule the server follows the client: once every detached
        // session is quiescent (and the heartbeat is stale), the linger expires
        // and the process shuts down. The close confirmation's "close anyway"
        // already TERMINATEd its sessions and erased those tabs, so this finds
        // nothing left for them; ctrl+w on an idle group and /detach-session end
        // their streams explicitly before erasing. DetachAllTabs is the catch-all
        // for whatever remains (sessionless tabs contribute nothing).
        app->DetachAllTabs();

        // Detach the focus seam BEFORE `app` unwinds: `app` (owning AppState) is
        // declared after `env` (owning the plugin registries and this provider),
        // so it is destroyed FIRST -- a plugin poller thread still calling
        // Tabs()/FocusedSessionId() during env's own teardown would otherwise
        // dereference a freed AppState. See HostFocusProvider::Detach().
        env.host_focus_provider->Detach();

        // Cancel subscribe streams BEFORE this function's locals unwind: `env`
        // (the plugin environment) is declared after `client`, so it is
        // destroyed FIRST, and a subscribing plugin that joins its worker in its
        // destructor would deadlock on a stream nobody has cancelled -- the
        // client's own destructor sweep only runs after env is already gone.
        client->CancelSubscriptions();
    };

    try {
        screen.Loop(final_tree);
    } catch (const std::exception& error) {
        shutdown_tui();
        minimal::common::DefaultLogger().error("tui: escaping exception: {}", error.what());
        std::fprintf(stderr, "minimal: internal error: %s\n", error.what());
        return kInternalError;
    } catch (...) {
        shutdown_tui();
        minimal::common::DefaultLogger().error("tui: escaping non-standard exception");
        std::fprintf(stderr, "minimal: internal error (non-standard exception)\n");
        return kInternalError;
    }
    shutdown_tui();
    return kOk;
}

}  // namespace minimal::client::app
