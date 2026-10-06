#pragma once

// The interactive TUI as an object (private to the `minimal` executable).
//
// Run() builds the environment, the frame, the modal stack and the key
// dispatch, then runs the FTXUI loop; the destructor undoes what Run started,
// in the order the members below document. RunTui (client_entry.hpp) is
// TuiApp(args).Run().
//
// MEMBER ORDER IS DESTRUCTION ORDER, and it is load-bearing: members are
// declared in the order RunTui once declared its locals, so they are
// destroyed in the reverse -- the frame before the state it reads, `app_`
// before `env_` (the plugin registries; see Shutdown()), `env_` before
// `client_`. Several members are std::optional only because they are built
// in a later phase than their declaration slot (their dependencies are
// members declared before them, but filled in by an earlier phase).
//
// Not movable or copyable: the views, pickers and key dispatch hold
// references into this object's own members.

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include "minimal/client/last_selection.hpp"
#include "minimal/client/cli_args.hpp"
#include "minimal/client/command_catalog.hpp"
#include "minimal/client/command_router.hpp"
#include "minimal/client/keybinding_dispatch.hpp"
#include "minimal/client/plugins/observer_events.hpp"
#include "minimal/client/session_client.hpp"
#include "minimal/client/state.hpp"
#include "minimal/client/tui_bootstrap.hpp"
#include "minimal/client/view/frame_renderer.hpp"
#include "minimal/client/view/modal_stack.hpp"
#include "minimal/client/view/picker_controller.hpp"
#include "minimal/client/view/prompt_editor.hpp"
#include "minimal/client/view/tab_ui.hpp"

namespace minimal::client::app {

// Also the TUI's own HostActions: what plugins queue (HostActionQueue) is
// applied to it on the UI thread.
class TuiApp final : private plugins::HostActions {
public:
    explicit TuiApp(const ParsedArgs& args) : args_(args) {}
    ~TuiApp() { Shutdown(); }

    TuiApp(const TuiApp&) = delete;
    TuiApp& operator=(const TuiApp&) = delete;

    // The whole TUI session; returns the process exit code.
    int Run();

private:
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
        // order is therefore safe either way.
        std::vector<std::string> tab_ids;
    };

    // --- startup phases, in Run()'s order ---
    // Connects and runs the startup --resume probe and the seed catalog fetch.
    int Connect();
    // Config, plugins, theme, keymap (tui_bootstrap.hpp). False: report and
    // exit.
    bool BuildEnvironment();
    // AppState, the command catalog cache, the startup resume, prompt
    // history, the focus seam.
    void BuildState();
    // The client-side /-command table (tui_commands.cpp).
    void RegisterClientCommands();
    void BuildEditor();
    void BuildModals();
    void BuildFrame();
    // The pump and heartbeat threads, and bracketed paste.
    void StartBackground();
    // Undoes whatever the phases built, in the order the body documents --
    // from the normal return, the loop's catch, and the destructor (so an
    // exception out of ANY phase still joins the threads and detaches the
    // seams before members unwind). Idempotent.
    void Shutdown();

    // --- operations (what RunTui's lambdas were) ---
    CommandCatalogCache::Snapshot CurrentCatalog();

    // HostActions, applied on the UI thread.
    void InsertPrompt(const std::string& text) override;
    // HostUi's reads (HostFocusProvider::AttachReads).
    std::vector<plugins::KeyBindingInfo> KeyBindingsForPlugins() const;
    std::vector<CommandCatalogEntry> CatalogForPlugins();
    AppState::CommandScope CommandScope() const;
    void CloseTab(const std::string& id);
    std::string FocusedOrNewTab();
    // The create options every new tab starts from: the cwd's project root,
    // plus --agent/--model when given, else this project's remembered last
    // pick (a soft preference the server may fall back from).
    SessionClient::CreateOptions NewTabOptions() const;
    void SubmitChat(const std::string& text);
    void SubmitCommand(const std::string& line);
    void ApplyTheme(const std::string& name);
    std::vector<std::string> ListThemeNames() const;
    void ClearFocusedNotice();
    void FocusedNotice(const std::string& text);
    void OpenNewSession();
    void OpenThemePicker();
    void OpenKeymapHelp();
    void RequestQuit();
    void StageClose(std::vector<std::string> ids, bool quit);
    void ToggleFold(const std::string& tab_id, bool thinking);
    ftxui::Component BuildPromptEditor(PromptEditorState& state);
    KeyContext MakeKeyContext();
    void RunPump();

    // --- members, in dependency (= declaration) order ---
    const ParsedArgs& args_;
    std::shared_ptr<SessionClient> client_;
    std::optional<v1::SessionSummary> startup_resume_;
    std::optional<std::vector<CommandCatalogEntry>> seed_catalog_;
    // MUST stay in place for the object's lifetime: several of its fields
    // hold pointers into ITS OWN other fields (tui_bootstrap.hpp's
    // address-stability contract).
    TuiEnvironment env_;

    std::shared_ptr<AppState> app_;
    // The pull-model catalog (see Connect's seed fetch). Destroyed before
    // `app_`; a fetch landing after that is dropped (the cache's completion
    // holds only a weak reference).
    std::optional<CommandCatalogCache> command_catalog_cache_;

    ftxui::ScreenInteractive screen_ = ftxui::ScreenInteractive::FullscreenAlternateScreen();
    std::function<void()> quit_;

    // --- frame state (loop thread only) ---
    PerTabPromptEditors prompt_editors_;
    PromptHistory prompt_history_;
    // The last agent/model picked per project (data dir); unset when the data
    // dir cannot be resolved -- picks are then not remembered.
    // Shared: AppState's hooks (run on the RPC worker too) keep it alive.
    std::shared_ptr<const LastSelectionStore> last_selection_;
    MentionQuerySlots mention_slots_;
    bool show_panel_ = false;
    // Every modal's visibility flag lives in the stack (view/modal_stack.hpp);
    // these names are references into it.
    ModalStack modals_;
    bool& show_picker_ = modals_.Flag(ModalId::kSessionPicker);
    bool& show_agent_picker_ = modals_.Flag(ModalId::kAgentPicker);
    bool& show_model_picker_ = modals_.Flag(ModalId::kModelPicker);
    bool& show_subagent_picker_ = modals_.Flag(ModalId::kSubagentPicker);
    bool& show_theme_picker_ = modals_.Flag(ModalId::kThemePicker);
    bool& show_keymap_help_ = modals_.Flag(ModalId::kKeymapHelp);
    bool& show_commands_help_ = modals_.Flag(ModalId::kCommandsHelp);
    bool& show_quit_confirm_ = modals_.Flag(ModalId::kQuitConfirm);
    bool& show_permission_ = modals_.Flag(ModalId::kPermission);
    bool& show_confirmation_ = modals_.Flag(ModalId::kConfirmation);
    bool& show_delete_confirm_ = modals_.Flag(ModalId::kDeleteConfirm);
    bool& show_plugin_modal_ = modals_.Flag(ModalId::kPluginModal);
    bool& show_prompt_ = modals_.Flag(ModalId::kPluginPrompt);
    std::string staged_delete_id_;   // the picker delete confirmation's subject
    PendingClose pending_close_;
    // Per-tab UI caches, keyed by TAB id (AppState's tab handle -- the only
    // key that exists for a sessionless tab). CloseTab erases from it.
    std::map<std::string, TabUi> tab_ui_;

    MentionSuggestor suggest_;
    CommandRouter command_router_;
    // Bracketed-paste flag, shared with HandleGlobalKey (keybinding_dispatch.cpp)
    // via KeyContext::pasting: a macOS Option-glyph byte pasted verbatim
    // (key_event_parser.cpp's IsOptionGlyphEvent) is byte-identical to the
    // key it would otherwise bind, so HandleGlobalKey needs to know a paste
    // is in progress to defer to the editor instead of firing the action.
    std::shared_ptr<bool> pasting_ = std::make_shared<bool>(false);
    ftxui::Component editor_;

    // --- modal slots: stable hosts whose inner component is swapped ---
    std::shared_ptr<ftxui::Component> permission_child_ = std::make_shared<ftxui::Component>();
    std::shared_ptr<ftxui::Component> confirmation_child_ = std::make_shared<ftxui::Component>();
    std::shared_ptr<ftxui::Component> prompt_child_ = std::make_shared<ftxui::Component>();
    std::shared_ptr<ftxui::Component> picker_child_ = std::make_shared<ftxui::Component>();
    std::shared_ptr<ftxui::Component> agent_picker_child_ = std::make_shared<ftxui::Component>();
    std::shared_ptr<ftxui::Component> model_picker_child_ = std::make_shared<ftxui::Component>();
    std::shared_ptr<ftxui::Component> subagent_picker_child_ =
        std::make_shared<ftxui::Component>();
    std::shared_ptr<ftxui::Component> theme_picker_child_ = std::make_shared<ftxui::Component>();
    std::shared_ptr<ftxui::Component> plugin_modal_child_ = std::make_shared<ftxui::Component>();
    PendingConfirmation confirm_request_;
    ftxui::Component confirm_modal_;
    std::optional<PickerController> picker_controller_;
    // Reset slots for the two help overlays: they are built once, so without
    // these the typed filter survived across opens.
    std::function<void()> keymap_help_reset_;
    std::function<void()> commands_help_reset_;
    ftxui::Component keymap_help_view_;
    ftxui::Component commands_help_view_;
    ftxui::Component plugin_modal_host_;

    // --- frame ---
    std::optional<plugins::ObserverPump> observer_pump_;
    // Never moved once built: Focused()/FocusedDetails() return references
    // into its own members, which the renderer and key dispatch hold.
    std::optional<TabMaintenance> tab_maintenance_;
    PendingConfirmation delete_request_;
    ftxui::Component delete_modal_;
    std::optional<FrameRenderer> frame_renderer_;
    std::optional<KeyContext> key_ctx_;
    // Coalescing for the pump's Event::Custom posts, cleared by the frame's
    // outermost CatchEvent (the one handler that sees every consumed event).
    std::atomic<bool> custom_event_in_flight_{false};
    ftxui::Component final_tree_;

    // --- background ---
    // Shutdown signal shared by both background threads: notified once,
    // right after running_ goes false, so a thread parked in a cv wait wakes
    // IMMEDIATELY instead of riding out its own poll interval.
    std::mutex shutdown_mtx_;
    std::condition_variable shutdown_cv_;
    std::atomic<bool> running_{false};
    std::thread pump_;
    std::thread heartbeat_;
    bool bracketed_paste_ = false;
    bool shut_down_ = false;
};

}  // namespace minimal::client::app
