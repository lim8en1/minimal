#pragma once

// What core registers itself: the plain-text markdown engine, the markdown
// slot's default when no markdown plugin holds it (markdown then reads as
// typed, never formatted). Every other default is a bundled plugin's.

#include "minimal/client/plugins/attention_state.hpp"
#include "minimal/client/plugins/plugin_view_entry.hpp"
#include "minimal/client/sdk/client_registries.hpp"

namespace minimal::client::plugins {

void RegisterPlainMarkdownEngine(MarkdownEngineRegistry& registry);

}  // namespace minimal::client::plugins
