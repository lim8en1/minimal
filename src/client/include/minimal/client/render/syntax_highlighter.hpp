#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <ftxui/dom/elements.hpp>

#include "minimal/client/render/theme.hpp"

namespace minimal::client {

enum class HighlightCategory { Keyword, String, Comment, Number, Function, Type, Operator, Plain };

// Byte range of `code` carrying one category. The testable half of the
// highlighter: span mapping is asserted here, FTXUI render output is not
// (ARCHITECTURE.md "Testing" -- visual, low yield).
struct HighlightSpan {
    HighlightCategory category;
    uint32_t start_byte;
    uint32_t end_byte;
};

// tree-sitter core plus a curated, vendored grammar set. A regex tokenizer was
// considered and rejected: nested/context-free constructs (Rust nested block
// comments, C++ raw strings, JS template literals) are exactly where correctness
// matters and exactly where regex fails.
//
// The highlighter never hardcodes a color; the theme supplies one per category.
class SyntaxHighlighter {
public:
    explicit SyntaxHighlighter(const Theme& theme);

    // Categorized spans over the whole code block, in preorder traversal order
    // (parents before their children). Plain spans are informational defaults,
    // not claims -- a Plain parent covers the whole input, so consumers must
    // skip Plain spans and apply "later wins" only among categorized ones; the
    // Highlight() method below is the reference implementation of that rule.
    // A language outside the curated set yields a single Plain span covering
    // the input.
    std::vector<HighlightSpan> HighlightCode(std::string_view code,
                                             std::string_view language) const;

    // One Element per line of the block (vbox-able), colors from the theme.
    // Per the documented signature; built on HighlightCode.
    ftxui::Elements Highlight(std::string_view code, std::string_view language) const;

    // Whether `language` resolves to a vendored grammar (aliases applied).
    static bool Supports(std::string_view language);

private:
    const Theme& theme_;
};

// Normalizes markdown fence aliases ("c++", "py", "sh", ...) to the canonical
// language name, or empty if unsupported. Exposed for the markdown renderer.
std::string CanonicalLanguage(std::string_view info_string_language);

}  // namespace minimal::client
