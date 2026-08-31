#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <tree_sitter/api.h>

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

// One rule: a tree-sitter node type and the category it maps to. Matches
// ANONYMOUS node types too -- tree-sitter keywords are anonymous nodes, which
// the rule tables exploit deliberately.
struct SyntaxRule {
    std::string_view node_type;
    HighlightCategory category;
};

// One language: canonical key, its parser, and its node-type table. The E5
// grammar registry hands these to the highlighter; the vendored set is the
// default resident.
using LanguageFnPtr = const TSLanguage* (*)(void);

struct LanguageDefinition {
    std::string_view name;
    LanguageFnPtr language_fn;
    std::span<const SyntaxRule> rules;
};

// The vendored grammar set, as injectable definitions -- what the host
// registers into the grammar registry as the defaults.
std::vector<LanguageDefinition> VendoredLanguages();

// tree-sitter core plus a curated, vendored grammar set. A regex tokenizer was
// considered and rejected: nested/context-free constructs (Rust nested block
// comments, C++ raw strings, JS template literals) are exactly where correctness
// matters and exactly where regex fails.
//
// The highlighter never hardcodes a color; the theme supplies one per category.
class SyntaxHighlighter {
public:
    explicit SyntaxHighlighter(const Theme& theme);
    // `languages` is the injected table (the E5 grammar registry's contents,
    // assembled by the host); the single-argument form uses the vendored set.
    explicit SyntaxHighlighter(const Theme& theme,
                               const std::vector<LanguageDefinition>& languages);

    // Categorized spans over the whole code block, in preorder traversal order
    // (parents before their children). Plain spans are informational defaults,
    // not claims -- a Plain parent covers the whole input, so consumers must
    // skip Plain spans and apply "later wins" only among categorized ones; the
    // Highlight() method below is the reference implementation of that rule.
    // A language outside the set yields a single Plain span covering the input.
    std::vector<HighlightSpan> HighlightCode(std::string_view code,
                                             std::string_view language) const;

    // One Element per line of the block (vbox-able), colors from the theme.
    // Per the documented signature; built on HighlightCode.
    ftxui::Elements Highlight(std::string_view code, std::string_view language) const;

    // Whether `language` resolves to a grammar in the active set (aliases
    // applied). With no table injected, the vendored set.
    bool Supports(std::string_view language) const;

private:
    const Theme& theme_;
    const std::vector<LanguageDefinition>* languages_ = nullptr;  // null = vendored
};

// Normalizes markdown fence aliases ("c++", "py", "sh", ...) to the canonical
// language name, or empty if unsupported. Exposed for the markdown renderer.
// With a table injected, plugin languages resolve exactly (no aliases).
std::string CanonicalLanguage(std::string_view info_string_language,
                              const std::vector<LanguageDefinition>* languages = nullptr);

}  // namespace minimal::client
