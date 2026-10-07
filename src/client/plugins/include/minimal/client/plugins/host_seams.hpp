#pragma once

#include <filesystem>
#include <functional>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

#include "minimal/client/plugins/attention_state.hpp"
#include "minimal/client/plugins/plugin_view_entry.hpp"
#include "minimal/client/sdk/client_registries.hpp"
#include "minimal/client/session_client.hpp"

namespace minimal::client {

class AppState;  // state.hpp; forward-declared to keep this header light

namespace plugins {

// Host-side implementations of the seams the registration context carries.
// The ABI names the capability; these own the wiring -- the same pattern as the
// server's context pointing at the host's ServiceRegistry.

// The AttentionSink handed to plugins, backed by the shared AttentionState
// block AppState reads. Null state = the seam is not offered (a sink that
// ignores its calls), which keeps a fixture plugin loadable without a TUI.
// `ui` (optional): every raise/clear also requests a redraw, so the tab bar
// shows the change now rather than at the next unrelated event.
// One queued HostUi view operation (see HostUi): produced on any thread by
// HostFocusProvider, applied on the UI thread (keybinding_dispatch.hpp
// ApplyViewRequest).
struct ViewRequest {
    enum class Op { kShow, kClose, kSetPanelVisible, kFocusPanel };

    ViewRequest() = default;
    ViewRequest(Op op, std::string view_id, bool visible = true, std::string plugin = {})
        : op(op), view_id(std::move(view_id)), visible(visible), plugin(std::move(plugin)) {}

    Op op = Op::kShow;
    std::string view_id;
    bool visible = true;   // kSetPanelVisible only
    // The requesting plugin, when it came through its bound HostUi (UiFor):
    // then `view_id` is that plugin's CANONICAL id, which still reaches its own
    // view after an alias renamed it. Empty: `view_id` is a surface name.
    std::string plugin;
};

std::unique_ptr<AttentionSink> MakeHostAttentionSink(AttentionState* state, HostUi* ui = nullptr);

// The ClientApi handed to plugins: scrollback pages over the host's ONE
// SessionClient, bounded like core's own refresh. Null client behaves the
// same way.
std::unique_ptr<ClientApi> MakeHostClientApi(SessionClient* client);

// The WidgetKit handed to plugins: forwards each call to whichever kit holds
// `registry` at that moment -- a plugin keeps this pointer from its
// registration context, and the kit (a plugin's, or core's default) is in
// place only once every plugin has registered.
std::unique_ptr<WidgetKit> MakeHostWidgetKit(const WidgetKitRegistry& registry);

// The MarkdownEngine handed to plugins, the same way: forwards to whichever
// engine holds `registry` at the call (core fills it after the plugins load).
std::unique_ptr<MarkdownEngine> MakeHostMarkdownEngine(const MarkdownEngineRegistry& registry);

// The FocusProvider handed to plugins: which tab/session the user is looking
// at, read from AppState's own thread-safe accessors. CREATED BEFORE AppState
// EXISTS -- the plugin-load step (BuildTuiEnvironment) precedes app
// construction, and reordering them would disturb the documented
// ORDER-MATTERS startup sequence -- so the target attaches once, after
// AppState is built. Until (and unless) it attaches, the seam refuses:
// FocusedSessionId() answers "" and Tabs() answers empty, which is exactly
// how the interface's own null-degrade rule reads to a plugin.
// It is also the host's HostUi (RequestRedraw): the same AppState target,
// the same Attach/Detach lifetime.
class HostFocusProvider final : public FocusProvider, public HostUi {
public:
    HostFocusProvider();
    ~HostFocusProvider() override;  // out of line: BoundUi is complete only there

    // Once, after AppState is constructed.
    void Attach(AppState* app);
    // Before AppState is destroyed: RunTui declares AppState's owner after
    // the plugin registries (this provider's owner), so locals unwind in
    // the reverse order -- AppState dies FIRST. A plugin poller thread still
    // calling Tabs()/FocusedSessionId() during the registries' own teardown
    // would otherwise dereference a freed AppState. Lock-guarded (not just
    // a null-out) so it acts as a real barrier: every call into app_ or the
    // UI reads holds the lock (shared) for its WHOLE call, so once Detach()
    // (exclusive) returns, no in-flight call is still touching either and
    // no new one can start. Drops the UI reads too: they reach into the
    // TUI's own members, which unwind before the plugins do.
    void Detach();

    std::string FocusedSessionId() const override;
    std::vector<TabInfo> Tabs() const override;
    // A shared hold like the reads, so it never waits on one (a Tabs() copy
    // on a poller thread); a no-op once Detach() returned.
    void RequestRedraw() override;
    void RequestRedrawAfter(std::chrono::milliseconds delay) override;
    // The pump's half: whether a RequestRedrawAfter deadline has come (and,
    // if so, forgets it).
    bool TakeRedrawDue();
    void ShowView(const std::string& view_id) override;
    void CloseView(const std::string& view_id) override;
    void SetPanelVisible(const std::string& view_id, bool visible) override;
    void FocusPanel(const std::string& view_id) override;
    std::vector<KeyBindingInfo> KeyBindings() const override;
    std::vector<CommandCatalogEntry> CommandCatalog() const override;
    std::vector<std::string> ThemeNames() const override;
    std::shared_ptr<const TabActivitySnapshot> Activity() const override;
    std::vector<ClientPluginInfo> ClientPlugins() const override;

    // The TUI's half of the reads, once it is built (after Attach); until
    // then, and after Detach, they answer empty. UI thread, like the reads
    // themselves.
    struct UiReads {
        std::function<std::vector<KeyBindingInfo>()> key_bindings;
        std::function<std::vector<CommandCatalogEntry>()> command_catalog;
        std::function<std::vector<std::string>()> theme_names;
        std::function<std::shared_ptr<const TabActivitySnapshot>()> activity;
        std::function<std::vector<ClientPluginInfo>()> client_plugins;
    };
    void AttachReads(UiReads reads);

    // The HostUi handed to one plugin: the same seam, with that plugin's name
    // stamped on its view requests. Owned here, stable for this provider's
    // lifetime.
    HostUi* UiFor(const std::string& plugin_name);

    // Where each plugin's LoadState/SaveState file lives (<dir>/<plugin>.json);
    // set before the plugins load. Unset: nothing is kept.
    void SetStateDir(std::filesystem::path dir) { state_dir_ = std::move(dir); }

    // The UI thread's half: every view request queued since the last call,
    // oldest first.
    std::vector<ViewRequest> TakeViewRequests();

private:
    class BoundUi;
    void Enqueue(ViewRequest request);

    std::mutex bound_mtx_;
    std::map<std::string, std::unique_ptr<BoundUi>> bound_;  // by plugin name
    std::filesystem::path state_dir_;

    // Shared by every read and RequestRedraw; exclusive for Attach,
    // AttachReads and Detach.
    mutable std::shared_mutex mtx_;
    UiReads reads_;            // guarded by mtx_; empty after Detach
    AppState* app_ = nullptr;  // guarded by mtx_; host-owned, null until Attach/after Detach
    // Its own lock, never held together with mtx_ (Enqueue releases it
    // before RequestRedraw takes mtx_).
    std::mutex requests_mtx_;
    std::vector<ViewRequest> requests_;
    // The earliest RequestRedrawAfter deadline (steady clock, ms); 0 = none.
    std::atomic<std::int64_t> redraw_due_ms_{0};
};

std::unique_ptr<HostFocusProvider> MakeHostFocusProvider();

// The CodeHighlighter handed to plugins: forwards to the host's highlighter
// once it is built (after the plugins load); until then code renders plain.
class HostCodeHighlighter final : public CodeHighlighter {
public:
    void Attach(const CodeHighlighter* target) { target_.store(target); }
    ftxui::Elements Highlight(std::string_view code, std::string_view language,
                              std::size_t block_id = 0) const override;
    std::string CanonicalLanguageFor(std::string_view language) const override;

private:
    std::atomic<const CodeHighlighter*> target_{nullptr};
};

// The HostActions handed to plugins: queues each verb (any thread) for the
// TUI to apply on the UI thread (Take, in the pump), and asks for the frame
// that will. `ui` (optional) is that redraw.
class HostActionQueue final : public HostActions {
public:
    explicit HostActionQueue(HostUi* ui) : ui_(ui) {}
    void InsertPrompt(const std::string& text) override;
    void SetTheme(const std::string& name) override;
    void ChooseAgent(const std::string& name) override;
    void ChooseModel(const std::string& ref) override;
    void ChooseDefaultModel(const std::string& default_ref) override;
    void SetSubagentModel(const std::string& ref) override;
    void NewTab() override;
    void OpenSession(const std::string& session_id, bool is_live) override;
    void OpenSubagent(const std::string& session_id, const std::string& parent_session_id,
                      bool is_live) override;
    void SetClientPluginEnabled(const std::string& id, bool enabled,
                                std::function<void(const std::string&)> done) override;

    // The UI thread's half: every verb queued since the last call, oldest
    // first, each to be applied to the TUI's own HostActions.
    std::vector<std::function<void(HostActions&)>> Take();

private:
    void Enqueue(std::function<void(HostActions&)> action);

    HostUi* ui_;
    std::mutex mtx_;
    std::vector<std::function<void(HostActions&)>> queued_;
};

}  // namespace plugins

}  // namespace minimal::client
