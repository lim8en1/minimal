#pragma once

// The bundled chat-renderers (plugins/client/chat-renderers) for tests that
// draw chat rows without caring how markdown looks: over core's plain engine.

#include "chat_renderers.hpp"

#include "minimal/client/plugins/core_registrations.hpp"
#include "minimal/client/plugins/attention_state.hpp"
#include "minimal/client/plugins/plugin_view_entry.hpp"
#include "minimal/client/sdk/client_registries.hpp"

namespace minimal_test {

inline void RegisterPlainChatRenderers(minimal::client::plugins::MessageRendererRegistry& registry) {
    static minimal::client::plugins::MarkdownEngineRegistry engines = [] {
        minimal::client::plugins::MarkdownEngineRegistry r;
        minimal::client::plugins::RegisterPlainMarkdownEngine(r);
        return r;
    }();
    minimal::client::chat_renderers::RegisterMessageRenderers(registry, *engines.get());
}

}  // namespace minimal_test
