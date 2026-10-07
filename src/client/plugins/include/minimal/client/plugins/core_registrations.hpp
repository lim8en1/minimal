#pragma once

#include <memory>
#include <vector>

#include "minimal/client/plugins/registries.hpp"
#include "minimal/client/render/syntax_highlighter.hpp"

namespace minimal::client::plugins {

// The core message-kind defaults -- the same presentation the message-kind
// table specifies, extracted from ChatView so a plugin can replace any ONE of
// them while the rest stand. The data plane (chat_model) stays untouched.
void RegisterCoreMessageRenderers(MessageRendererRegistry& registry);

// The defaults that render through the markdown engine -- the assistant's
// and the user's messages, and compaction summaries -- registered once the
// engine is settled (a replacement engine flows through them).
void RegisterCoreMarkdownRenderers(MessageRendererRegistry& registry,
                                   const MarkdownEngine& engine);

// Renderers for the core display-payload kinds (diff, file, process,
// markdown), filling only the kinds no plugin claimed. The engine and the
// highlighter must outlive the registry (both live in TuiEnvironment).
void RegisterCoreDisplayRenderers(DisplayRendererRegistry& registry, const MarkdownEngine& engine,
                                  const SyntaxHighlighter& highlighter);


// Registers the plain-text engine when no plugin claimed the markdown slot
// (it is optional: without a markdown plugin, markdown reads as plain text).
void RegisterPlainMarkdownEngine(MarkdownEngineRegistry& registry);

}  // namespace minimal::client::plugins
