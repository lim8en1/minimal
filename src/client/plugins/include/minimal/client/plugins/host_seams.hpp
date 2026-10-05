#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "minimal/client/plugins/registries.hpp"
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
    // would otherwise dereference a freed AppState. Mutex-guarded (not just
    // a null-out) so it acts as a real barrier: FocusedSessionId()/Tabs()
    // hold the same lock for their WHOLE call into app_, so once Detach()
    // returns, no in-flight call is still touching app_ and no new one can
    // start.
    void Detach();

    std::string FocusedSessionId() const override;
    std::vector<TabInfo> Tabs() const override;
    // Held under the same lock as the reads: a no-op once Detach() returned.
    void RequestRedraw() override;
    void ShowView(const std::string& view_id) override;
    void CloseView(const std::string& view_id) override;
    void SetPanelVisible(const std::string& view_id, bool visible) override;
    void FocusPanel(const std::string& view_id) override;

    // The HostUi handed to one plugin: the same seam, with that plugin's name
    // stamped on its view requests. Owned here, stable for this provider's
    // lifetime.
    HostUi* UiFor(const std::string& plugin_name);

    // The UI thread's half: every view request queued since the last call,
    // oldest first.
    std::vector<ViewRequest> TakeViewRequests();

private:
    class BoundUi;
    void Enqueue(ViewRequest request);

    std::mutex bound_mtx_;
    std::map<std::string, std::unique_ptr<BoundUi>> bound_;  // by plugin name

    mutable std::mutex mtx_;
    AppState* app_ = nullptr;  // guarded by mtx_; host-owned, null until Attach/after Detach
    // Its own lock, never held together with mtx_ (Enqueue releases it
    // before RequestRedraw takes mtx_).
    std::mutex requests_mtx_;
    std::vector<ViewRequest> requests_;
};

std::unique_ptr<HostFocusProvider> MakeHostFocusProvider();

}  // namespace plugins

}  // namespace minimal::client
