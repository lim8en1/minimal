#pragma once

#include <memory>
#include <optional>

#include "minimal/client/plugins/registries.hpp"
#include "minimal/client/session_client.hpp"

namespace minimal::client::plugins {

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

}  // namespace minimal::client::plugins
