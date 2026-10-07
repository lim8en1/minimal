#pragma once

// chat-renderers -- how each kind of chat row and display payload looks: the
// user's and the assistant's messages (through the markdown engine),
// thinking, tool activity, attachments, system notes, switches, compactions;
// diffs, files, processes and markdown payloads. Every one is a FALLBACK
// registration: a plugin claiming one kind replaces just that one.

#include "minimal/client/sdk/registries.hpp"
#include "minimal/client/sdk/highlighter.hpp"

namespace minimal::client::chat_renderers {

namespace plugins = minimal::client::plugins;

void RegisterMessageRenderers(plugins::MessageRendererRegistry& registry,
                              const plugins::MarkdownEngine& engine);

void RegisterDisplayRenderers(plugins::DisplayRendererRegistry& registry,
                              const plugins::MarkdownEngine& engine,
                              const CodeHighlighter& highlighter);

}  // namespace minimal::client::chat_renderers
