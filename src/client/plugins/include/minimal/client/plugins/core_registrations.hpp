#pragma once

#include <memory>
#include <vector>

#include "minimal/client/plugins/registries.hpp"
#include "minimal/client/render/markdown_renderer.hpp"
#include "minimal/client/render/syntax_highlighter.hpp"

namespace minimal::client::plugins {

// The core message-kind defaults -- the same presentation the message-kind
// table specifies, extracted from ChatView so a plugin can replace any ONE of
// them while the rest stand. The data plane (chat_model) stays untouched.
void RegisterCoreMessageRenderers(MessageRendererRegistry& registry);

// The agent-markdown default, registered once the engine is settled (it
// renders through the engine, so a replacement engine flows through it).
void RegisterCoreAgentMarkdownRenderer(MessageRendererRegistry& registry,
                                       const MarkdownEngine& engine);

// The vendored grammars as the additive registry's defaults. Always runs --
// the grammar slot has no "core default" concept beyond the vendored set; a
// plugin may only ADD a language (claiming an existing one is a fatal
// collision).
void RegisterCoreSyntaxGrammars(SyntaxGrammarRegistry& registry);

// Registers the md4c engine when no plugin claimed the exclusive slot.
void RegisterDefaultMarkdownEngine(MarkdownEngineRegistry& registry,
                                   const MarkdownRenderer& renderer);

}  // namespace minimal::client::plugins
