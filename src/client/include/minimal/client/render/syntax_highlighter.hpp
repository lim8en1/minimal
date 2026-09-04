#pragma once

#include <cstdint>
#include <list>
#include <map>
#include <memory>
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

// Flattens preorder spans (HighlightCode's output: parents before children,
// so a parent's range covers its descendants) into ordered, non-overlapping
// runs where a DEEPER node wins over its ancestors: a keyword inside a JS
// template literal renders as Keyword, not as the literal's String. Bytes no
// span claims -- and gaps -- come back Plain. Exposed for the span-level
// tests, which is where the override rule is assertable.
std::vector<HighlightSpan> ApplySpanOverrides(std::string_view code,
                                              const std::vector<HighlightSpan>& spans);

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
    ~SyntaxHighlighter();

    // Categorized spans over the whole code block, in preorder traversal order
    // (parents before their children). NOT a paint order: a parent's range
    // covers its descendants, and which one wins is ApplySpanOverrides's rule
    // (deeper wins) -- the reference consumer. Plain spans are informational:
    // named nodes not in the rule table. A language outside the set yields a
    // single Plain span covering the input.
    std::vector<HighlightSpan> HighlightCode(std::string_view code,
                                             std::string_view language) const;

    // Internal render path: identical traversal, Plain spans omitted (the
    // Element renderer never consumed them).
    std::vector<HighlightSpan> Collect(std::string_view code, std::string_view language,
                                       bool include_plain) const;
    // One Element per line of the block (vbox-able), colors from the theme.
    // Per the documented signature; built on HighlightCode.
    ftxui::Elements Highlight(std::string_view code, std::string_view language) const;

    // Whether `language` resolves to a grammar in the active set (aliases
    // applied). With no table injected, the vendored set.
    bool Supports(std::string_view language) const;
private:
    const Theme& theme_;
    const std::vector<LanguageDefinition>* languages_ = nullptr;  // null = vendored

    // -- Render-path caches (track R3). The highlighter is used from the FTXUI
    // render loop ONLY (single-threaded); Highlight()/HighlightCode() are const
    // because the Caches are an implementation detail, not because they are
    // thread-safe.
    //
    // One TSParser per language, created once and reused. The first draft
    // created + destroyed a parser PER HIGHLIGHT CALL -- which the streaming
    // path pays every 120 ms debounce tick for every code block in the buffer.
    // Key: canonical language name; empty entries for languages that failed to
    // load (negative cache).
    class ParserCache;
    std::unique_ptr<ParserCache> parser_cache_;

    // Whole-result cache, keyed on (language, code): a COMPLETED code block
    // (the common case once a stream's cursor has moved past it) is
    // byte-identical between reparses, so every 120 ms debounce tick was
    // re-running the tree-sitter traversal AND rebuilding every ftxui::text
    // Element for a block that had not changed at all -- the dominant cost of
    // a long, code-heavy streaming message, growing linearly through it as
    // more blocks finished. MRU-ordered, bounded (kHighlightCacheCap): a
    // streaming buffer holds only a handful of distinct blocks at once, so a
    // small cap is a real bound, not a knob anyone needs to tune. ftxui::Element
    // is a shared_ptr-backed handle (ChatView's own row cache relies on the
    // same fact), so returning a cached Elements vector by copy is cheap.
    struct HighlightCacheEntry {
        std::string language;
        std::string code;
        ftxui::Elements lines;
    };
    static constexpr std::size_t kHighlightCacheCap = 64;
    mutable std::list<HighlightCacheEntry> highlight_cache_;   // MRU at front
};

// Normalizes markdown fence aliases ("c++", "py", "sh", ...) to the canonical
// language name, or empty if unsupported. Exposed for the markdown renderer.
// With a table injected, plugin languages resolve exactly (no aliases).
std::string CanonicalLanguage(std::string_view info_string_language,
                              const std::vector<LanguageDefinition>* languages = nullptr);

}  // namespace minimal::client
