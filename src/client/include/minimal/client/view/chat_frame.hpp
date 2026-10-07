#pragma once

// The chat surface's input (registries.hpp ChatFrame), built from a tab's
// deep snapshot: what the chat view used to read from SessionState directly.

#include "minimal/client/plugins/attention_state.hpp"
#include "minimal/client/plugins/plugin_view_entry.hpp"
#include "minimal/client/sdk/client_registries.hpp"
#include "minimal/client/state.hpp"

namespace minimal::client {

// Cheap: the transcript window, the compactions, the live text and the tool
// inputs are shared pointers; only the small per-segment records are built.
plugins::ChatFrame ChatFrameFrom(const SessionState::Snapshot& snapshot);

}  // namespace minimal::client
