#pragma once

#include <memory>
#include <optional>
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
std::unique_ptr<AttentionSink> MakeHostAttentionSink(AttentionState* state);

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
class HostFocusProvider final : public FocusProvider {
public:
    // Once, after AppState is constructed; never re-attached, never detached.
    void Attach(AppState* app);

    std::string FocusedSessionId() const override;
    std::vector<TabInfo> Tabs() const override;

private:
    AppState* app_ = nullptr;  // host-owned; null until Attach
};

std::unique_ptr<HostFocusProvider> MakeHostFocusProvider();

}  // namespace plugins

}  // namespace minimal::client
