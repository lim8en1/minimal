#pragma once

#include <string_view>

#include <ftxui/dom/elements.hpp>

#include "minimal/client/sdk/highlighter.hpp"
#include "minimal/client/sdk/theme.hpp"

namespace minimal::client::markdown_md4c {


// md4c (SAX, GFM) mapped onto ftxui::Elements by a hand-rolled adapter -- the
// same call made for openai-cpp: own the boundary rather than depend on a thin,
// unmaintained integration layer (MarkdownFTXUI was rejected for this).
//
// Render output is NOT unit-tested (visual, low yield -- ARCHITECTURE.md
// "Testing"); the logic that would be (span mapping, debounce) lives in
// SyntaxHighlighter::HighlightCode and StreamingMarkdownView.
class MarkdownRenderer {
public:
    // Both may be omitted: no theme -> default terminal colors, no highlighter
    // -> fenced code renders uncolored. References must outlive the renderer.
    explicit MarkdownRenderer(const Theme* theme = nullptr,
                              const CodeHighlighter* highlighter = nullptr);

    // Parses with MD_DIALECT_GITHUB (tables, strikethrough, tasklists, and the
    // permissive autolink rules chat text actually contains).
    ftxui::Element Render(std::string_view markdown_text) const;

private:
    const Theme* theme_;
    const CodeHighlighter* highlighter_;
};

}  // namespace minimal::client::markdown_md4c
