// The interactive TUI (TuiApp, tui_app.hpp): builds the environment, the
// frame, the modal stack and the key dispatch, then runs the FTXUI loop. The
// client-side /-command table lives in tui_commands.cpp.

#include "tui_app.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <type_traits>

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include "client_entry.hpp"
#include "minimal/client/config_file_io.hpp"
#include "minimal/client/plugins/plugin_discovery.hpp"
#include "minimal/client/sdk/chat_model.hpp"
#include "minimal/client/sdk/policy.hpp"
#include "minimal/client/plugins/host_seams.hpp"
#include "minimal/client/sdk/theme.hpp"
#include "minimal/client/ui_config.hpp"
#include "minimal/client/keymap_help_model.hpp"
#include "minimal/client/view/modal_host.hpp"
#include "minimal/common/config_paths.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/client/policy.hpp"

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
// That worker is joined explicitly (app_->StopRpcQueue(), in Shutdown, right
// after the pump) rather than left detached, for the same teardown-ordering
// reason the reader/writer threads are joined rather than detached: a job's
// completion touches state that DetachAllTabs() is about to tear down.
//
// Keybindings: every action is dispatched through keybindings.json (the file
// is the config); the table below only maps a key NAME to the byte sequences a
// terminal may send for it. A name the terminal cannot encode matches nothing,
// which is exactly what remapping exists for -- and what the startup warning
// names.

std::vector<CommandCatalogEntry> ToCatalogEntries(const v1::ListCommandsResponse& response) {
    std::vector<CommandCatalogEntry> entries;
    for (const auto& info : response.commands()) {
        entries.push_back({info.name(), info.summary(), info.read_only(),
                           info.kind() == v1::COMMAND_KIND_PROMPT_SHORTHAND,
                           info.requires_session(), info.sensitive_args()});
    }
    return entries;
}

}  // namespace

int RunTui(const ParsedArgs& args) { return TuiApp(args).Run(); }

SessionClient::CreateOptions TuiApp::NewTabOptions() const {
    auto opts = DefaultCreateOptions();
    if (!args_.headless.agent_preset.empty() || !args_.headless.model.empty()) {
        // Explicit flags are a decision made now: sent as such, never
        // softened, and the remembered pick is not consulted.
        opts.agent_preset = args_.headless.agent_preset;
        opts.model = args_.headless.model;
        return opts;
    }
    if (last_selection_) {
        const auto last = last_selection_->For(opts.project_root);
        opts.agent_preset = last.agent;
        opts.model = last.model;
        opts.remembered_selection = !last.agent.empty() || !last.model.empty();
    }
    return opts;
}

int TuiApp::Run() {
    if (const int code = Connect(); code != kOk) return code;
    if (!BuildEnvironment()) {
        Shutdown();
        return kUnavailable;
    }
    BuildState();
    RegisterClientCommands();
    BuildEditor();
    BuildModals();
    BuildFrame();
    StartBackground();

    try {
        screen_.Loop(final_tree_);
    } catch (const std::exception& error) {
        Shutdown();
        common::DefaultLogger().error("tui: escaping exception: {}", error.what());
        std::fprintf(stderr, "minimal: internal error: %s\n", error.what());
        return kInternalError;
    } catch (...) {
        Shutdown();
        common::DefaultLogger().error("tui: escaping non-standard exception");
        std::fprintf(stderr, "minimal: internal error (non-standard exception)\n");
        return kInternalError;
    }
    Shutdown();
    return kOk;
}

// --- startup ------------------------------------------------------------------

int TuiApp::Connect() {
    const std::string target =
        args_.headless.server.empty() ? DefaultServerTarget() : args_.headless.server;

    // The same connect (auto-spawn first, 5 s wait) as the headless paths.
    client_ = ConnectOrReport(target, args_.headless.autospawn,
                              args_.headless.allow_version_mismatch);
    if (!client_) return kUnavailable;

    // The startup --resume probe: resolve the target session BEFORE the UI is
    // built, so a bad id fails fast with a clear line instead of an empty TUI.
    // What is kept from the summary is exactly what the attach step needs:
    // liveness (attach vs revive) and the subagent refusal (a subagent's life
    // is one turn of its parent; opening one standalone has no meaning -- the
    // server refuses its resume too).
    if (!args_.headless.resume_session_id.empty()) {
        const auto found = client_->GetSession(args_.headless.resume_session_id);
        if (found.status.error_code() == grpc::StatusCode::NOT_FOUND) {
            std::fprintf(stderr, "no such session: %s\n",
                         args_.headless.resume_session_id.c_str());
            std::fprintf(stderr, "list stored sessions with: minimal --list-sessions\n");
            return kUnavailable;
        }
        if (!found.status.ok()) {
            std::fprintf(stderr, "could not look up session %s: %s\n",
                         args_.headless.resume_session_id.c_str(),
                         found.status.error_message().c_str());
            return kUnavailable;
        }
        if (!found.summary.parent_session_id().empty()) {
            std::fprintf(stderr, "session %s is a subagent of %s and cannot be opened "
                                 "standalone; open the parent session instead\n",
                         args_.headless.resume_session_id.c_str(),
                         found.summary.parent_session_id().c_str());
            return kUnavailable;
        }
        startup_resume_ = found.summary;
    }

    // The /-command catalog: SEEDED here with one synchronous fetch at
    // connect (the startup project root), so the very first `/` has
    // something to show; from then on PULLED per tab scope by
    // CommandCatalogCache (BuildState) -- every read answers from the last
    // known list and, at most every 2 s, refetches in the background. So a
    // command file added to the project mid-run appears the next time
    // autocomplete or /help opens, and a tab in a different project lists
    // that project's commands.
    const auto listed = client_->ListCommands(CurrentProjectRootOrEmpty());
    if (listed.status.ok()) seed_catalog_ = ToCatalogEntries(listed.response);
    return kOk;
}

bool TuiApp::BuildEnvironment() {
    // ORDER MATTERS -- see tui_bootstrap.hpp/.cpp for the full sequence
    // (ui.json -> grammars -> plugin load -> collisions -> theme ->
    // keybindings -> status format -> highlighter/renderer/engine -> chat
    // surface). On failure, plugins loaded before it may already hold
    // subscribe streams on worker threads: Shutdown() cancels them so
    // `env_`'s teardown (its feeds join those workers) cannot wait on a
    // stream nobody ends.
    return BuildTuiEnvironment(*client_, env_);
}

void TuiApp::BuildState() {
    app_ = std::make_shared<AppState>(client_);
    app_->SetPluginAttention(&env_.attention_state);

    command_catalog_cache_.emplace(
        CommandCatalogCache::Entries{},   // the client's own: SetClientSide, once the router is built
        [app = app_](const std::string& session_id, const std::string& project_root,
                     std::function<void(std::optional<CommandCatalogCache::Entries>)> done) {
            app->RequestListCommands(
                session_id, project_root,
                [done = std::move(done)](std::optional<v1::ListCommandsResponse> response) {
                    if (!response) return done(std::nullopt);
                    done(ToCatalogEntries(*response));
                });
        });

    // THE STARTUP RESUME, on the probed session (Connect's probe, before the
    // UI was built). LIVE opens the whole tab group -- the session plus every
    // live subagent child (AttachSessionGroup); anything else REVIVES the
    // stored transcript into a fresh root group (ResumeTab), the picker's own
    // stale-liveness fallthrough. "" from the live path means the session
    // ended between probe and attach; resume is then the remaining option,
    // and its failure is visible as an Error tab, never silent. Either way
    // the tab focuses itself (focus-if-empty), so no separate SetFocusedTab
    // is needed here.
    if (startup_resume_) {
        const std::string session_id = startup_resume_->session_id();
        if (startup_resume_->is_live()) {
            if (!app_->AttachSessionGroup(session_id).empty()) startup_resume_ = std::nullopt;
        }
        if (startup_resume_) (void)app_->ResumeTab(session_id);
    }
    // The focus seam's target, now that AppState exists (host_seams.hpp:
    // plugin load ran before app construction, so the provider attaches here).
    // Plugins captured the provider POINTER at registration; anything they
    // read through it happens later, so the attach is never raced.
    env_.host_focus_provider->Attach(app_.get());
    // Its reads too: they answer from TUI state built from here on, and are
    // only ever called on the UI thread.
    env_.host_focus_provider->AttachReads(
        {.key_bindings = [this] { return KeyBindingsForPlugins(); },
         .command_catalog = [this] { return CatalogForPlugins(); },
         .theme_names = [this] { return ListThemeNames(); },
         .activity = [this] { return activity_feed_.Get(*app_); },
         .client_plugins = [this] { return env_.client_plugins; }});
    // Memory bounds (track R2): ui.json's values, or the built-in defaults
    // (SetUiBounds treats 0 as "keep the default"). Applied before any tab can
    // open, so every SessionState starts bounded.
    app_->SetUiBounds(env_.ui.scrollback_window_max_messages, env_.ui.tool_output_buffer_bytes);

    // FULLSCREEN ALTERNATE SCREEN (screen_), not TerminalOutput: the
    // alternate buffer is repaint-safe on terminal resize. TerminalOutput
    // repaints the PRIMARY screen by cursor arithmetic over the previous
    // frame, and a resize reflows the text ftxui still thinks it owns --
    // every repaint then re-stamps stale frame fragments (old tab-bar lines,
    // typed text) as "junk" above the live frame. Any out-of-band
    // stdout/stderr write would desync it the same way.
    //
    // ftxui 7 force-handles Ctrl+C by default -- it exits the loop and
    // re-raises SIGINT EVEN when a component consumes the event -- which
    // defeats this client's busy-cancel and quit confirmation. The client's
    // own dispatch (keybinding_dispatch.cpp) must see Ctrl+C first.
    screen_.ForceHandleCtrlC(false);
    quit_ = screen_.ExitLoopClosure();

    // Up-arrow recall survives restarts (data dir, beside the client log).
    try {
        prompt_history_.PersistTo(common::DataDir() / "prompt_history.json");
    } catch (const std::exception& error) {   // no $HOME: recall stays per-run
        common::DefaultLogger().warn("prompt history not persisted: {}", error.what());
    }

    // The last agent/model pick, per project: recorded whenever the user
    // stages one (picker or /agent, /model), applied to every new tab, and
    // forgotten when the server can no longer honor it.
    try {
        last_selection_ =
            std::make_shared<const LastSelectionStore>(common::DataDir() / "last_selection.json");
    } catch (const std::exception& error) {
        common::DefaultLogger().warn("last agent/model pick not remembered: {}", error.what());
    }
    if (last_selection_) {
        const std::string root = CurrentProjectRootOrEmpty();
        std::shared_ptr<const LastSelectionStore> store = last_selection_;
        app_->SetSelectionHooks(AppState::SelectionHooks{
            [store, root](const std::optional<std::string>& agent,
                          const std::optional<std::string>& model) {
                store->Record(root, agent, model);
            },
            [store = std::move(store), root](bool agent, bool model) {
                store->Forget(root, agent, model);
            },
        });
    }

    // T2: @-mention suggestions used to run SuggestPaths synchronously, on
    // the loop thread, from inside the prompt editor's own Renderer -- a
    // merely slow server stalled every keystroke after '@' for up to the
    // RPC's 250 ms deadline. The suggestor's contract (prompt_editor.hpp's
    // MentionSuggestor) is "nullopt = still waiting, poll again next frame";
    // the mechanics live in MakeMentionSuggestor (prompt_editor.cpp): one
    // in-flight slot PER TAB (MentionQuerySlots). Sessioned tabs query by
    // session id (the server resolves the root); a sessionless tab scopes to
    // the TUI's cwd, the root its first message would create the session
    // with.
    suggest_ = MakeMentionSuggestor(app_.get(), mention_slots_, [](const std::string&) {
        return CurrentProjectRootOrEmpty();
    });
}

void TuiApp::BuildEditor() {
    // The catalog lists the table's own commands after every server list;
    // seeded only now that the table is complete.
    command_catalog_cache_->SetClientSide(command_router_.CatalogEntries());
    if (seed_catalog_) {
        command_catalog_cache_->Seed({}, CurrentProjectRootOrEmpty(), std::move(*seed_catalog_));
        seed_catalog_.reset();
    }

    // Per-tab prompt editors (prompt_host.hpp): the slot's factory makes one
    // per tab -- a draft, its suggestion list and debounce are THAT tab's --
    // all over this one host (every call acts on the focused tab) and the
    // ONE shared recall timeline. The host component swaps the focused tab's
    // editor in live, at render AND event time (a key in the same frame as a
    // tab switch lands in the tab focused at event time -- the rule the
    // submit paths follow via FocusedOrNewTab); "" serves the empty app.
    prompt_history_.SetCatalog([this] { return CurrentCatalog(); });
    editor_actions_ = env_.registries.prompt_editor.get()->Actions();
    for (auto& action : env_.chat_surface->Actions()) {
        chat_action_ids_.push_back(action.id);
        editor_actions_.push_back(std::move(action));
    }
    prompt_editors_.SetBuilder([this] {
        return env_.registries.prompt_editor.get()->Make(*this, *env_.theme);
    });
    editor_ = MakePerTabPromptEditor(prompt_editors_, [this] { return app_->FocusedTabId(); });
}

// --- PromptEditorHost ---------------------------------------------------------

std::shared_ptr<const std::vector<CommandCatalogEntry>> TuiApp::Commands() {
    const auto catalog = CurrentCatalog();
    if (catalog == nullptr) return nullptr;
    const bool sessioned = !app_->FocusedSessionId().empty();
    const bool subagent = app_->FocusedTabIsSubagent();
    // Rebuilt only when an input changed: the editor polls this per frame
    // and reads a new pointer as a new list.
    if (catalog != editor_catalog_.source || sessioned != editor_catalog_.sessioned ||
        subagent != editor_catalog_.subagent || editor_catalog_.usable == nullptr) {
        editor_catalog_ = {catalog, sessioned, subagent,
                           std::make_shared<const std::vector<CommandCatalogEntry>>(
                               CatalogForPlugins())};
    }
    return editor_catalog_.usable;
}

std::optional<std::vector<std::string>> TuiApp::Mentions(const std::string& prefix) {
    return suggest_(prefix);
}

bool TuiApp::Matches(std::string_view action, const ftxui::Event& event) const {
    return env_.keymap.Matches(action, event);
}

void TuiApp::RequestRedraw() { env_.host_focus_provider->RequestRedraw(); }

// --- ChatHost -------------------------------------------------------------------

const plugins::MessageRendererRegistry& TuiApp::MessageRenderers() const {
    return env_.registries.message_renderers;
}
const plugins::ToolResultRendererRegistry& TuiApp::ToolRenderers() const {
    return env_.registries.tool_renderers;
}
const plugins::DisplayRendererRegistry& TuiApp::DisplayRenderers() const {
    return env_.registries.display_renderers;
}
const plugins::MarkdownEngine& TuiApp::Markdown() const { return *env_.engine; }
std::string TuiApp::KeyFor(std::string_view action) const {
    return env_.keybindings.KeyFor(std::string(action));
}
void TuiApp::CancelQueued(const std::string& tab_id, std::uint64_t turn_id) {
    // The tab's session, resolved at click time (a pane may predate it).
    app_->SendCancelQueuedTurnByTab(tab_id, turn_id);
}
void TuiApp::Notice(const std::string& tab_id, const std::string& text) {
    app_->TabNotice(tab_id, text);
}

std::unique_ptr<plugins::ChatPane> TuiApp::MakeChatPane(const std::string& tab_id) {
    std::unique_ptr<plugins::ChatPane> pane;
    GuardChat([&] { pane = env_.chat_surface->Make(*this, tab_id, *env_.theme); });
    return pane;
}

plugins::ChatPane* TuiApp::PaneOf(const std::string& tab_id) {
    const auto it = tab_ui_.find(tab_id);
    return it != tab_ui_.end() ? it->second.chat.get() : nullptr;
}

void TuiApp::GuardChat(const std::function<void()>& call) {
    // Plugin code: a throw is logged and that one call dropped, never the TUI.
    try {
        call();
    } catch (const std::exception& error) {
        common::DefaultLogger().error("chat view threw: {}", error.what());
    } catch (...) {
        common::DefaultLogger().error("chat view threw a non-standard exception");
    }
}

void TuiApp::BuildModals() {
    using namespace ftxui;
    const Theme& theme = *env_.theme;

    // --- permission / confirmation / plugin prompt ------------------------------
    // Each host is a STABLE component in the tree; its inner component is
    // rebuilt (TabMaintenance) when a new request_id arrives. Checkbox state
    // lives in GrantToggles (keyed by request in tab_ui_), so it survives the
    // rebuild. MakeModalHost binds the show flag by reference -- a by-value
    // capture latched `false` forever once: the prompt rendered but no key
    // ever reached it (the "frozen UI" class). Same for the picker hosts.
    //
    // The server confirmation modal (/clear asks through the
    // ConfirmationBroker, which BLOCKS the session worker until answered) and
    // the model's ask modal (tool-ask plugin) nest directly below the
    // permission prompt in the stack -- the three pending kinds are mutually
    // exclusive in SessionState, so they never compete for the layer.
    modals_.Set(ModalId::kPermission, MakeModalHost(permission_child_, show_permission_));
    modals_.Set(ModalId::kConfirmation, MakeModalHost(confirmation_child_, show_confirmation_));
    modals_.Set(ModalId::kPluginPrompt, MakeModalHost(prompt_child_, show_prompt_));

    // --- quit / close confirmation --------------------------------------------
    // A stable host; StageClose builds a fresh modal into it each time.
    modals_.Set(ModalId::kQuitConfirm, MakeModalHost(quit_confirm_child_, show_quit_confirm_));

    // The pickers are plugin views (sessions, subagents, agents, models,
    // subagent-models); core keeps what their choices do.
    picker_controller_.emplace(PickerController::Dependencies{
        *client_, *app_,
        // The desired-state choices stage onto this -- the focused tab, or
        // (the empty startup app, zero tabs) a fresh sessionless one, so a
        // /agent //model choice made before any tab exists lands on the tab
        // the first message will use.
        [this] { return FocusedOrNewTab(); },
        [this](const std::string& role) { return OpenRoleView(role); }});

    // --- plugin views (panel region + modal layer) -----------------------------
    // Panels: rendered in the frame beside the tools panel; one shared
    // CatchEvent (over the frame) routes events to visible ones. Modals: one
    // host in the modal stack BELOW the quit confirmation -- a plugin modal
    // can never preempt the security UI, which is the invariant the nesting
    // itself enforces (view/modal_stack.hpp).
    //
    // The host closes it on Escape (ClosePluginModal) and contains the
    // plugin's throws -- see MakePluginModalHost.
    plugin_modal_host_ = MakePluginModalHost(plugin_modal_child_, show_plugin_modal_, [this] {
        ClosePluginModal(env_.plugin_views, plugin_modal_child_, show_plugin_modal_);
    });
    modals_.Set(ModalId::kPluginModal, plugin_modal_host_);
}

void TuiApp::BuildFrame() {
    using namespace ftxui;
    Theme& theme = *env_.theme;
    auto& registries = env_.registries;

    // Plugin observers' events, derived between frames (observer_events.hpp).
    observer_pump_.emplace(*app_, registries.observers);

    // THE MAINTENANCE PASS: everything the frame needs but must not do from
    // Render -- a fresh focused-tab snapshot, the per-tab streaming/refresh
    // bookkeeping, and the once-per-notice VIEW side-effects (a View-display
    // notice may stage a modal and run a bounded ListSessions RPC; doing
    // that inside the renderer made the frame stall and the staged modal
    // unclosable).
    tab_maintenance_.emplace(TabMaintenance::Dependencies{
        *app_, registries.views, env_.plugin_views, plugin_modal_child_,
        show_plugin_modal_,
        // Loop-thread work queued from other threads: picker fetch results,
        // plugins' HostUi view requests (show/close/focus), and the plugin
        // observers' events.
        [this] {
            picker_controller_->Poll();
            for (auto& action : env_.host_actions->Take()) action(*this);
            for (const auto& request : env_.host_focus_provider->TakeViewRequests()) {
                const bool was_modal = show_plugin_modal_;
                ApplyViewRequest(env_.plugin_views, request, *plugin_modal_child_,
                                 show_plugin_modal_);
                // An overlay opening sets no notice of its own: a stale one
                // (the last command's) must not sit under it and after it.
                if (show_plugin_modal_ && !was_modal) ClearFocusedNotice();
            }
            picker_controller_->WatchPendingView(ShownPluginModalRole());
            observer_pump_->Run();
        },
        tab_ui_, theme, registries.prompt_renderers, permission_child_, show_permission_,
        confirmation_child_, show_confirmation_, prompt_child_, show_prompt_,
        [this](const std::string& tab_id) { return MakeChatPane(tab_id); }});

    // FrameRenderer owns the tab body / status-bar / plugin-panel assembly.
    const int panel_height = env_.ui.panel_height > 0 ? env_.ui.panel_height : 12;   // ui.json
    frame_renderer_.emplace(FrameRenderer::Dependencies{
        *app_, theme, tab_maintenance_->Focused(), tab_maintenance_->FocusedDetails(),
        tab_maintenance_->FocusedChatFrame(), tab_ui_,
        registries.start_views, registries.status_segments, registries.tab_bar, panel_height,
        editor_, env_.status_bar.format, env_.status_segment_ids, env_.plugin_views,
        // The key the empty state names: the user's keymap decides it.
        env_.keybindings.KeyFor("session.picker", "ctrl+o"),
        [this](const std::string& tab_id, TabUi& ui) {
            tab_maintenance_->EnsureChatPane(tab_id, ui);
        }});
    auto session_view = Renderer(editor_, [this] { return frame_renderer_->Render(); });

    // --- global keybindings -----------------------------------------------------
    // The dispatch body lives in keybinding_dispatch.cpp (HandleGlobalKey);
    // the context is built ONCE, not per event.
    key_ctx_.emplace(MakeKeyContext());
    auto with_keys = CatchEvent(session_view, [this](const Event& event) {
        return HandleGlobalKey(*key_ctx_, event);
    });

    // The modal stack (view/modal_stack.hpp owns the order: the security
    // prompts topmost, plugin modals lowest). Rooted at with_keys, NOT raw
    // session_view: E5's refactor once re-rooted it at session_view and
    // silently dropped every global keybinding.
    const Component with_modals = modals_.Build(with_keys);

    // The maintenance pass rides the event path ABOVE the whole stack: this
    // handler sees every event the loop processes (including the pump's
    // Event::Custom, even while a modal would consume ordinary keys), runs
    // the maintenance pass, and consumes NOTHING -- a pure pre-render
    // observation point. This is what keeps side-effects out of Render().
    final_tree_ = CatchEvent(with_modals, [this](const Event&) {
        // This event is consumed and its frame is about to render -- the
        // pump may queue its next post. Set here (not at post time) so at
        // most one un-consumed Custom is ever outstanding: a frame render
        // that cannot keep pace must not find a fresh post queued every 50ms
        // tick (each queued Custom renders one FULL frame).
        custom_event_in_flight_.store(false, std::memory_order_release);
        tab_maintenance_->Run();
        // A paste can only be IN PROGRESS while the editor is the stack's
        // active leaf; any modal grabbing exclusive routing (FTXUI Modal()'s
        // own contract) swallows the rest of the paste, including its own
        // end marker, before the editor's CatchEvent (prompt_editor.cpp)
        // ever sees it to clear this itself. Self-heal here, the one place
        // that watches every event above the whole stack: the first event
        // after a modal opens mid-paste clears the flag, rather than leaving
        // it stuck true (every later Return silently inserting a newline
        // instead of submitting). tab_maintenance_->Run() above already
        // applied this tick's snapshot, so a permission/confirmation/ask
        // modal that just became due is reflected in its show flag by now.
        if (modals_.AnyOpen() && prompt_editors_.Built(app_->FocusedTabId())) {
            if (auto* editor = prompt_editors_.Editor(app_->FocusedTabId())) editor->OnFocusLost();
        }
        return false;
    });
    // The theme's optional canvas color, outermost so it reaches every modal
    // too. Read per frame: /theme swaps the theme in place. A theme without
    // `background` leaves the terminal's own (possibly transparent) bg.
    final_tree_ = Renderer(final_tree_, [inner = final_tree_, &theme] {
        return inner->Render() | FillBackground(theme.ColorFor("background"));
    });
    tab_maintenance_->Run();   // the first frame must not render an empty snapshot
}

KeyContext TuiApp::MakeKeyContext() {
    return KeyContext{
        *app_, env_.keymap, tab_maintenance_->Focused(), show_plugin_modal_,
        env_.plugin_views, plugin_modal_child_,
        [this] { OpenRoleView("sessions"); },
        [this] { picker_controller_->OpenAgentPicker(); },
        [this] { picker_controller_->OpenModelPicker(); },
        [this] { RequestQuit(); },
        [this](const std::string& id) { CloseTab(id); },
        // tab.close with work in flight: stage ONE confirmation for the
        // whole closing group, same as RequestQuit's own busy branch.
        [this](std::vector<std::string> ids) { StageClose(std::move(ids), /*quit=*/false); },
        chat_action_ids_,
        [this](const std::string& tab_id, std::string_view action) {
            if (auto* pane = PaneOf(tab_id)) GuardChat([&] { pane->OnAction(action); });
        },
        [this](const std::string& tab_id, const ftxui::Event& event) {
            auto* pane = PaneOf(tab_id);
            bool consumed = false;
            if (pane != nullptr) GuardChat([&] { consumed = pane->OnMouse(event); });
            return consumed;
        },
        // The focused editor's own keys outrank every binding (Claims).
        [this](const ftxui::Event& event) {
            const std::string tab = app_->FocusedTabId();
            if (!prompt_editors_.Built(tab)) return false;
            auto* editor = prompt_editors_.Editor(tab);
            return editor != nullptr && editor->Claims(event);
        },
        [this](int x, int y) { return frame_renderer_->TabAt(x, y); }};
}

void TuiApp::StartBackground() {
    running_.store(true, std::memory_order_release);
    pump_ = std::thread([this] { RunPump(); });

    // THE HEARTBEAT: the server's idle exit (server.idle_shutdown_seconds,
    // default 5s) fires once zero sessions are live AND zero streams are
    // attached AND no recent Ping arrived. A TUI on its empty-state welcome
    // view -- before its first session, which is created lazily on the first
    // submission -- holds neither, so without a beat the server shut itself
    // down while the user was still composing that first message. One Ping
    // per ~2s marks the client alive; quitting the TUI stops the beats and
    // the server exits on its own linger as designed. A condition_variable
    // wait: Shutdown's notify ends it at once.
    heartbeat_ = std::thread([this] {
        std::unique_lock<std::mutex> lock(shutdown_mtx_);
        while (!shutdown_cv_.wait_for(lock, std::chrono::seconds(2), [this] {
            return !running_.load(std::memory_order_acquire);
        })) {
            (void)client_->Ping();   // failures ignored -- see SessionClient::Ping
        }
    });

    // Bracketed paste (DECSET 2004): enabled for the duration of the loop, the
    // terminal wraps a paste in ESC[200~ ... ESC[201~ markers. ftxui 7 has no
    // paste support, but it forwards the markers verbatim as Event::Special
    // (unknown CSI -> Special, full bytes preserved), which the prompt editor
    // consumes to treat a multi-line paste as ONE turn instead of one
    // submission per line. Shutdown turns it off, on every exit path.
    std::fputs("\x1b[?2004h", stdout);
    std::fflush(stdout);
    bracketed_paste_ = true;
}

// THE PUMP: reader threads wake nothing by themselves; this loop redraws at up
// to 20 Hz from snapshots. Redraws are gated on the app's UI revision -- an
// idle TUI used to repaint the full frame 20x per second no matter what. The
// 50ms wake itself is not optional (nothing else can notice a UiVersion()
// change or the shutdown flag). The forced-redraw floor is gated on what
// actually needs time to pass with no version bump: the editor's @-mention
// debounce (query_pending_hint -- it fires FROM a render pass), the streaming
// frames plugins asked for ahead of time (HostUi::RequestRedrawAfter -- the
// markdown engine's debounce: a burst of deltas that PAUSES wakes this pump
// once for the last delta, and nothing bumps the version when the debounce
// window closes), a quiet running tool's
// elapsed counter, a busy tab's spinner through a silent stretch, and a due
// scrollback retry.
void TuiApp::RunPump() {
    uint64_t last_version = app_->UiVersion();
    // Running-tool forced redraws are bucketed to whatever FormatDuration can
    // actually display (tool_status.hpp) instead of firing every 50ms tick: a
    // repaint pass is a full FTXUI tree walk + full-screen terminal write, so
    // forcing it 20x/s for the whole life of a long tool call paid real CPU
    // for ticks the display couldn't show. Sentinel far in the past so the
    // very first running tick fires immediately.
    auto last_running_redraw = std::chrono::steady_clock::now() - std::chrono::hours(1);
    // When a frame last went out, for any reason. The spinner heartbeat only
    // fills SILENT gaps: streaming deltas and running-tool redraws already
    // repaint, and each repaint pushes this forward.
    auto last_post = std::chrono::steady_clock::now();
    while (running_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(kUiTick);
        const uint64_t version = app_->UiVersion();
        // A foreground tool that goes quiet (a long `sleep`, a silent build
        // step) produces no wire events, so nothing bumps UiVersion -- without
        // this, both the spinner and the elapsed counter would freeze. <1s
        // shows whole ms (the full 50ms cadence), 1s-60s tenths (100ms),
        // >=60s whole seconds (1000ms).
        bool running_tool_redraw_due = false;
        if (const auto elapsed = app_->FocusedTabFastestRunningToolElapsedMs()) {
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
        // A thinking model streams nothing through its prefill, so nothing
        // bumps UiVersion and the spinner (status line, tab bar) would freeze
        // exactly when "still alive?" is the question. One frame per spinner
        // step, and only once nothing else repainted for that long -- the time
        // check first, so the tab scan runs only when a frame is due.
        const bool spinner_redraw_due =
            std::chrono::steady_clock::now() - last_post >= client::kSpinnerFrameInterval &&
            app_->AnyTabBusy();
        const bool debounce_pending =
            prompt_editors_.WantsFrames(app_->FocusedTabId()) ||
            env_.host_focus_provider->TakeRedrawDue() ||
            running_tool_redraw_due || spinner_redraw_due ||
            tab_maintenance_->RefreshRetryDue();
        if (version != last_version || debounce_pending) {
            // Coalesce: skip the post while the previous Custom is still
            // unconsumed. last_version moves ONLY on a successful post, so a
            // skipped tick re-posts (with the newer version) on the next one
            // -- worst case one pump tick of extra latency, never a stale
            // frame.
            if (!custom_event_in_flight_.exchange(true, std::memory_order_acq_rel)) {
                last_version = version;
                last_post = std::chrono::steady_clock::now();
                screen_.PostEvent(ftxui::Event::Custom);
            }
        }
    }
}

void TuiApp::Shutdown() {
    if (shut_down_) return;
    shut_down_ = true;

    if (running_.load(std::memory_order_acquire)) {
        {
            // Under shutdown_mtx_, not a bare store: the heartbeat's
            // wait_for(lock, 2s, predicate) checks the predicate WHILE holding
            // this same lock, so a store+notify landing between its check and
            // its block could otherwise be missed (an extra 2s of quit delay).
            std::lock_guard<std::mutex> lk(shutdown_mtx_);
            running_.store(false, std::memory_order_release);
        }
        shutdown_cv_.notify_all();
    }
    if (pump_.joinable()) pump_.join();
    if (heartbeat_.joinable()) heartbeat_.join();

    if (app_) {
        // Join the async RPC worker BEFORE tearing down tabs: a job's
        // completion (RequestScrollback et al.) publishes into
        // SessionState/AppState the same way a reader thread's dispatch does,
        // and DetachAllTabs() below destroys the very tabs a straggling job
        // would publish into. The join is unbounded against whatever job is
        // currently running (a blocking RPC with its own multi-second
        // deadline), so say so rather than sit there looking hung.
        if (app_->RpcQueueBusy()) {
            std::fprintf(stderr, "minimal: waiting for a pending request to finish...\n");
        }
        app_->StopRpcQueue();

        // S5.4: quitting the TUI DETACHES every remaining tab rather than
        // terminating it -- a session outlives the client that opened it, so
        // closing the terminal must not destroy in-flight work. The close
        // confirmation's "close anyway" already TERMINATEd its sessions and
        // erased those tabs; DetachAllTabs is the catch-all for whatever
        // remains (sessionless tabs contribute nothing).
        app_->DetachAllTabs();
    }

    // Detach the focus seam BEFORE `app_` unwinds: `app_` is declared after
    // `env_` (owning the plugin registries and this provider), so it is
    // destroyed FIRST -- a plugin poller thread still calling
    // Tabs()/FocusedSessionId() during env_'s own teardown would otherwise
    // dereference a freed AppState. See HostFocusProvider::Detach().
    if (env_.host_focus_provider) env_.host_focus_provider->Detach();
    // The highlighter proxy forwards into env_.highlighter, which unwinds
    // before the registries (and the plugin threads they join) do: plain
    // text from here on.
    if (env_.host_highlighter) env_.host_highlighter->Attach(nullptr);

    // Cancel subscribe streams BEFORE the members unwind: `env_` is declared
    // after `client_`, so it is destroyed FIRST, and a subscribing plugin
    // that joins its worker in its destructor would deadlock on a stream
    // nobody has cancelled -- the client's own destructor sweep only runs
    // after env_ is already gone.
    if (client_) client_->CancelSubscriptions();

    if (bracketed_paste_) {
        std::fputs("\x1b[?2004l", stdout);
        std::fflush(stdout);
        bracketed_paste_ = false;
    }
}

// --- operations ----------------------------------------------------------------

AppState::CommandScope TuiApp::CommandScope() const {
    // The focused tab's scope: its session, else its pending project root (a
    // tab-less app scopes like a fresh tab would, to the TUI's cwd).
    auto scope = app_->FocusedCommandScope();
    if (scope.session_id.empty() && scope.project_root.empty()) {
        scope.project_root = CurrentProjectRootOrEmpty();
    }
    return scope;
}

bool TuiApp::OpenRoleView(const std::string& role) {
    // By role, never by name: an alias renaming the view must not break
    // /agent, /model or a first message waiting on a choice.
    for (auto& entry : env_.plugin_views) {
        if (entry.view->spec().role != role) continue;
        ShowPluginView(env_.plugin_views, entry, *plugin_modal_child_, show_plugin_modal_);
        return true;
    }
    FocusedNotice("nothing to choose with: no view is registered as '" + role +
                  "' (a bundled plugin provides it -- is it disabled in client-plugins.json?)");
    return false;
}

std::string TuiApp::ShownPluginModalRole() const {
    if (!show_plugin_modal_) return {};
    for (const auto& entry : env_.plugin_views) {
        if (entry.visible && entry.view->spec().placement == plugins::ViewPlacement::Modal) {
            return entry.view->spec().role;
        }
    }
    return {};
}

void TuiApp::ChooseAgent(const std::string& name) { picker_controller_->ChooseAgent(name); }
void TuiApp::ChooseModel(const std::string& ref) { picker_controller_->ChooseModel(ref); }
void TuiApp::ChooseDefaultModel(const std::string& default_ref) {
    picker_controller_->ChooseDefaultModel(default_ref);
}
void TuiApp::SetSubagentModel(const std::string& ref) { picker_controller_->SetSubagentModel(ref); }
void TuiApp::OpenSession(const std::string& session_id, bool is_live) {
    picker_controller_->OpenSession(session_id, is_live);
}
void TuiApp::OpenSubagent(const std::string& session_id, const std::string& parent, bool is_live) {
    picker_controller_->OpenSubagent(session_id, parent, is_live);
}

void TuiApp::SetClientPluginEnabled(const std::string& id, bool enabled,
                                    std::function<void(const std::string&)> done) {
    // Every refusal is said twice: the focused tab's notice, and to the
    // caller (a view open over the tabs hides the notice).
    const auto refuse = [this, &done](const std::string& why) {
        FocusedNotice(why);
        if (done) done(why);
    };
    const auto info = std::find_if(env_.client_plugins.begin(), env_.client_plugins.end(),
                                   [&id](const auto& plugin) { return plugin.id == id; });
    if (info == env_.client_plugins.end()) {
        refuse("no client plugin '" + id + "'");
        return;
    }
    if (!enabled && !info->required.empty()) {
        // Swapping in a replacement is one file edit, both entries at once:
        // two claimants of a required slot refuse the next start, and no
        // claimant does too, so neither half can go through here alone.
        refuse("cannot turn " + info->name + " off: " + info->required +
               " -- the TUI would not start without it. To replace it, in client-plugins.json "
               "set \"enabled\": false for \"" + id +
               "\" and true for your replacement, then restart");
        return;
    }
    // Re-read: the file may have been edited since startup. One that cannot
    // be read, or used as written, is not rewritten over the user's bytes --
    // "could not read it" must never become "it was empty".
    const auto path = env_.config_dir / "client-plugins.json";
    std::string contents;
    std::error_code exists_ec;
    if (std::filesystem::exists(path, exists_ec) && !TryReadConfigFile(path, &contents)) {
        refuse("client-plugins.json could not be read -- not changed");
        return;
    }
    plugins::ClientPluginConfig file = plugins::ParseClientPluginConfig(contents);
    if (file.malformed || !file.problems.empty()) {
        refuse("client-plugins.json cannot be used as written -- fix it first" +
               (file.problems.empty() ? std::string() : ": " + file.problems.front()));
        return;
    }
    const auto provenance = env_.client_plugin_provenance.at(id);
    bool found = false;
    for (auto& entry : file.plugins) {
        if (entry.file != id || (entry.provenance && *entry.provenance != provenance)) continue;
        entry.provenance = provenance;
        entry.enabled = enabled;
        found = true;
    }
    if (!found) file.plugins.push_back({id, provenance, enabled});
    if (!WriteConfigFileAtomic(path, plugins::SerializeClientPluginConfig(file))) {
        refuse("could not write " + path.string());
        return;
    }
    info->enabled_in_file = enabled;
    FocusedNotice(info->name + (enabled ? " is on" : " is off") +
                  (enabled == info->enabled ? "" : " from the next TUI start"));
    if (done) done("");
}

void TuiApp::InsertPrompt(const std::string& text) {
    // The focused tab's editor (CurrentState("") on an empty app = the
    // pre-tab editor, the same resolution the host itself uses).
    if (auto* editor = prompt_editors_.Editor(app_->FocusedTabId())) editor->SetText(text);
}

std::vector<plugins::KeyBindingInfo> TuiApp::KeyBindingsForPlugins() const {
    std::vector<plugins::KeyBindingInfo> out;
    for (const auto& [id, key] : env_.keybindings.all()) {
        // From 1 within the declaring slot's own list (the editor's, then the
        // chat surface's -- one vector, split at chat_action_ids_).
        std::optional<std::size_t> rank;
        const std::size_t chat_start = editor_actions_.size() - chat_action_ids_.size();
        for (std::size_t i = 0; i < editor_actions_.size(); ++i) {
            if (editor_actions_[i].id == id) rank = (i < chat_start ? i : i - chat_start) + 1;
        }
        KeyActionInfo info = DescribeKeyAction(id, rank);
        // A plugin view's toggle: its own title says what it opens.
        for (const auto& entry : env_.plugin_views) {
            if (entry.surface_id == id) info.description = "open " + entry.view->spec().title;
        }
        // The prompt editor's actions: its own words.
        for (const auto& action : editor_actions_) {
            if (action.id == id) info.description = action.description;
        }
        out.push_back({id, key, std::move(info.description), std::move(info.group), info.order});
    }
    return out;
}

std::vector<CommandCatalogEntry> TuiApp::CatalogForPlugins() {
    // The autocomplete's own filter: without a session only what runs
    // without one, on a subagent only what is read-only there.
    const bool sessioned = !app_->FocusedSessionId().empty();
    const bool subagent = app_->FocusedTabIsSubagent();
    std::vector<CommandCatalogEntry> out;
    if (const auto catalog = CurrentCatalog()) {
        for (const auto& entry : *catalog) {
            if (!sessioned && !UsableWithoutSession(entry)) continue;
            if (subagent && !UsableOnSubagent(entry)) continue;
            out.push_back(entry);
        }
    }
    return out;
}

CommandCatalogCache::Snapshot TuiApp::CurrentCatalog() {
    const auto scope = CommandScope();
    return command_catalog_cache_->Get(scope.session_id, scope.project_root);
}

// Tab close, for every caller (keybinding, quit confirmation, quit,
// /detach-session): the per-tab UI caches must die WITH the tab -- tab_ui
// entries were never erased once, leaking each closed tab's streaming buffer,
// grants and surface body for the process lifetime.
void TuiApp::CloseTab(const std::string& id) {
    app_->CloseTab(id);
    if (const auto it = tab_ui_.find(id); it != tab_ui_.end() && frame_renderer_) {
        frame_renderer_->ForgetPluginCall(it->second.chat.get());
    }
    tab_ui_.erase(id);
    // The tab's editor dies WITH the tab (state AND component together -- the
    // component's closures captured the state's address; neither may outlive
    // the other). The "" pre-tab entry is never forgotten.
    prompt_editors_.ForgetTab(id);
    mention_slots_.Forget(id);
}

// Tabs are sessionless until the first message: OpenTab mints a TAB (a
// client-side container, no RPC, no server row); the session comes into
// existence only when a message is submitted (SubmitChatTurn ->
// AppState::EnsureSession, single-flight per tab on the RPC worker). Nothing
// reaches the server until there is something to say, and a tab abandoned
// before that leaves no row at all.
std::string TuiApp::FocusedOrNewTab() {
    if (std::string tab = app_->FocusedTabId(); !tab.empty()) return tab;
    return app_->OpenTab(NewTabOptions());   // local-only; focuses itself
}

void TuiApp::SubmitChat(const std::string& text) {
    const std::string tab = FocusedOrNewTab();
    // A new session with no agent/model chosen and no default to fall back
    // on asks first (the pickers), then sends; dismissing a picker puts the
    // message back in the prompt.
    picker_controller_->EnsureSelectionThen(
        tab,
        [this, tab, text] {
            // Sending a turn returns the view to the newest row: scrolled-up
            // content would otherwise sit frozen while the reply streams in
            // somewhere below the visible viewport. Same for a row cursor left
            // over from browsing. (A sessionless tab has no chat view yet.)
            if (auto* pane = PaneOf(tab)) GuardChat([&] { pane->OnSubmitted(); });
            app_->SubmitChatTurn(tab, text);
        },
        [this, tab, text] {
            auto* editor = prompt_editors_.Editor(tab);
            if (editor != nullptr && editor->Text().empty()) editor->SetText(text);
        });
}

// Everything the client's own table does not handle. SESSIONLESS TABS: a
// command the server catalog marks requires_session is refused client-side,
// visibly, without creating a session; the genuinely sessionless ones ride
// RunSessionlessCommand, and prompt-submitting ones (markdown commands) start
// the session themselves.
void TuiApp::SubmitCommand(const std::string& line) {
    if (command_router_.Dispatch(line)) return;
    if (!app_->FocusedSessionId().empty()) {
        app_->SubmitCommand(app_->FocusedTabId(), line);
        return;
    }
    // A name ABSENT from the catalog is reported as what it is -- unknown --
    // rather than riding the session notice (a removed command reading as
    // "start a session first" was exactly the /providers confusion). Only
    // when the catalog actually arrived, though: after a failed ListCommands
    // it is empty and knows nothing.
    const auto parsed = SplitCommand(line);
    bool known = false;
    bool sessionless = false;
    bool submits_prompt = false;
    const auto scope = CommandScope();
    const bool commands_listed = command_catalog_cache_->Listed(scope.session_id, scope.project_root);
    for (const auto& entry : *CurrentCatalog()) {
        if (entry.name == parsed.name) {
            known = true;
            sessionless = !entry.requires_session;
            submits_prompt = entry.submits_prompt;
            break;
        }
    }
    const std::string tab = FocusedOrNewTab();
    if (tab.empty()) return;
    if (!known && commands_listed) {
        app_->TabNotice(tab, "unknown command: /" + parsed.name + " (try /help)");
    } else if (known && !sessionless && submits_prompt) {
        // A prompt command starts the session, like a first message: the
        // command parks on the tab, EnsureSession creates the session with
        // the staged model/agent, then the command runs on the new stream.
        app_->SubmitCommand(tab, line);
    } else if (!sessionless) {
        app_->TabNotice(tab, "/" + parsed.name + " needs a session; send a message to start one first");
    } else {
        app_->RunSessionlessCommand(tab, line);
    }
}

// THE THEME SWAP PATH, shared by "/theme <name>" and the picker's Enter:
// replace the theme's CONTENTS in place (the object keeps its address -- every
// view, the syntax highlighter and the markdown renderer hold references to it
// and re-read ColorFor per frame, so the next paint is already the new
// palette), drop the highlighter's color-baked cache, re-push OnTheme to the
// theme-aware plugins, and record the choice in ui.json for the next launch. A
// failed load changes nothing and reports the error through the tab notice --
// a typo'd name must not blank the UI.
void TuiApp::ApplyTheme(const std::string& name) {
    Theme loaded;
    try {
        loaded = Theme::Load(env_.config_dir / "themes", name, env_.depth, &env_.loaded_plugins);
    } catch (const std::exception& error) {
        FocusedNotice("theme: " + std::string(error.what()));
        return;
    }
    *env_.theme = std::move(loaded);
    env_.highlighter->OnThemeChanged();
    PushThemeToPlugins(env_);
    SaveThemePreference(env_.config_dir, name);
    FocusedNotice("theme switched to '" + name + "'");
}

// The picker's row set: the stems of <config_dir>/themes/*.json. Sorted so the
// menu is stable across opens; a missing/unreadable dir yields an empty list
// (the picker's empty_message covers it).
std::vector<std::string> TuiApp::ListThemeNames() const {
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(env_.config_dir / "themes", ec)) {
        std::error_code entry_ec;
        if (!entry.is_regular_file(entry_ec) || entry.path().extension() != ".json") continue;
        names.push_back(entry.path().stem().string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

void TuiApp::ClearFocusedNotice() {
    if (const std::string tab = app_->FocusedTabId(); !tab.empty()) app_->ClearTabNotice(tab);
}

void TuiApp::FocusedNotice(const std::string& text) {
    if (const std::string tab = app_->FocusedTabId(); !tab.empty()) app_->TabNotice(tab, text);
}

// The picker's "+ new session" row: the same action as typing on an empty
// app, reached from inside the modal with no text to send yet. The tab it
// opens is SESSIONLESS, so an abandoned "+ new" tab leaves no row behind.
void TuiApp::NewTab() {
    if (const auto tab = app_->OpenTab(NewTabOptions()); !tab.empty()) {
        app_->SetFocusedTab(tab);
    }
}

// Quit = close every tab, with ONE confirmation if any work is in flight
// (ARCHITECTURE.md "Session lifetime"). Shared by ctrl+q and
// ctrl+c-when-idle (raw mode means ctrl+c reaches the loop as a key event,
// not as SIGINT).
void TuiApp::RequestQuit() {
    const auto tabs = app_->GetAllTabs();
    std::vector<std::string> ids;
    for (const auto& tab : tabs) ids.push_back(tab.tab_id);
    // HasQueuedWork: a sessionless tab's EnsureSession job can still be
    // QUEUED (not yet started, so its state hasn't moved off Idle --
    // AnyWorkInFlight can't see it) when quit is pressed. StopRpcQueue
    // DISCARDS the queue rather than draining it, so without this check a
    // message typed moments before quit was silently dropped.
    if (!AnyWorkInFlight(tabs, ids) && !app_->HasQueuedWork()) {
        quit_();
    } else {
        StageClose(std::move(ids), /*quit=*/true);
    }
}

ftxui::Component TuiApp::MakeQuitConfirm() {
    using namespace ftxui;
    // THREE answers, not two (the close-vs-detach split): "close anyway"
    // destroys the sessions and everything in flight (EndSession{TERMINATE});
    // "detach" ends only the streams -- the sessions keep running
    // server-side, work finishes unattended, the server shuts itself down
    // once nothing is busy; "cancel" (esc) backs out.
    // Drawn by the "confirmation" prompt renderer like the server's own
    // confirmations, built fresh per ask (a renderer answers once); anything
    // but one of the three options is "cancel".
    static const std::vector<std::string> kQuitOptions = {"close anyway", "cancel", "detach"};
    const auto on_quit_choice = [this](const std::string& chosen) {
        if (chosen != "close anyway" && chosen != "detach") {
            show_quit_confirm_ = false;
            pending_close_ = PendingClose{};
            return;
        }
        // State the intent on the wire FIRST, while the streams are still
        // open: DETACH or TERMINATE per the answer. Then close the tabs root
        // first, children after (the staging order -- see PendingClose); each
        // Close() lands on an already-ended stream and degrades to a no-op.
        app_->EndTabSessions(pending_close_.tab_ids, chosen == "detach"
                                                         ? EndSessionMode::kDetach
                                                         : EndSessionMode::kTerminate);
        for (const auto& id : pending_close_.tab_ids) CloseTab(id);
        if (pending_close_.quit) quit_();
        show_quit_confirm_ = false;
        pending_close_ = PendingClose{};
    };
    auto modal = MakeConsentModal(
        env_.registries.prompt_renderers, "confirmation",
        ConfirmationPayload("app_quit", "work is still running",
                            "a session still has work in flight. close: destroy it now. "
                            "detach: keep it running in the background (/sessions reattaches).",
                            kQuitOptions),
        "", *env_.theme, [on_quit_choice](const plugins::PromptReply& reply) {
            on_quit_choice(ConfirmationChoice(kQuitOptions, reply));
        });
    // Startup refuses without a "confirmation" renderer; if it threw here,
    // a bare layer that Escape still cancels -- never one that traps input.
    if (modal == nullptr) {
        modal = CatchEvent(Renderer([] {
                                        return text("quit confirmation unavailable -- esc: cancel") |
                                               border;
                                    }),
                                    [on_quit_choice](const Event& event) {
                                        if (event != Event::Escape) return true;
                                        on_quit_choice("cancel");
                                        return true;
                                    });
    }
    return modal;
}

void TuiApp::StageClose(std::vector<std::string> ids, bool quit) {
    pending_close_ = PendingClose{quit, std::move(ids)};
    *quit_confirm_child_ = MakeQuitConfirm();
    show_quit_confirm_ = true;
}


}  // namespace minimal::client::app
