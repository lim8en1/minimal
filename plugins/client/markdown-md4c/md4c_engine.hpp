#pragma once

// markdown-md4c -- the bundled markdown engine (registries.hpp
// MarkdownEngine): md4c parsing, fenced code colored by the host's
// highlighter, and streaming text reparsed on a debounce -- partial input
// cannot soundly parse, so a growing message is reparsed at most once per
// window and Finalize() always reparses authoritatively.

#include "minimal/client/plugins/registries.hpp"

namespace minimal::client::markdown_md4c {

// Registers the engine into `registry`. `highlighter` colors fenced code
// (null: code renders plain); `ui` brings the frame a debounced reparse is
// due in (null: the next event does).
void RegisterEngine(plugins::MarkdownEngineRegistry& registry, const CodeHighlighter* highlighter,
                    plugins::HostUi* ui);

}  // namespace minimal::client::markdown_md4c
