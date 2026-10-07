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

#include "minimal/client/sdk/highlighter.hpp"
#include "minimal/client/sdk/theme.hpp"
#include "minimal/client/policy.hpp"

namespace minimal::client {

// Byte range of `code` carrying one category. The testable half of the
// highlighter: span mapping is asserted here, FTXUI render output is not
// (ARCHITECTURE.md "Testing" -- visual, low yield).
struct HighlightSpan {
    HighlightCategory category;
    uint32_t start_byte;
    uint32_t end_byte;
};

// Flattens preorder spans (HighlightCode's output: parents before children,
// so a parent's range covers its descendants) into ordered, non-overlapping
// runs where a DEEPER node wins over its ancestors: a keyword inside a JS
// template literal renders as Keyword, not as the literal's String. Bytes no
// span claims -- and gaps -- come back Plain. Exposed for the span-level
// tests, which is where the override rule is assertable.
std::vector<HighlightSpan> ApplySpanOverrides(std::string_view code,
                                              const std::vector<HighlightSpan>& spans);

class SyntaxHighlighter final : public CodeHighlighter {
public:
    // `languages` is the grammar registry's contents, assembled by the host;
    // it must outlive the highlighter.
    explicit SyntaxHighlighter(const Theme& theme,
                               const std::vector<LanguageDefinition>& languages);
    ~SyntaxHighlighter();

    // Categorized spans over the whole code block, in preorder traversal order
    // (parents before their children). NOT a paint order: a parent's range
    // covers its descendants, and which one wins is ApplySpanOverrides's rule
    // (deeper wins) -- the reference consumer. Plain spans are informational:
    // named nodes not in the rule table. A language outside the set yields a
    // single Plain span covering the input.
    //
    // `block_id`, when nonzero, identifies the calling code block within its
    // document (e.g. a fence's ordinal position) so the incremental parse
    // cache below can keep one base tree per BLOCK, not per language -- two
    // same-language blocks sharing one slot otherwise thrash each other's
    // incremental base every debounce tick. 0 (the default) preserves the
    // original language-only keying for callers with no block identity to
    // offer (tests, ad hoc callers).
    std::vector<HighlightSpan> HighlightCode(std::string_view code, std::string_view language,
                                             std::size_t block_id = 0) const;

    // Internal render path: identical traversal, Plain spans omitted (the
    // Element renderer never consumed them).
    std::vector<HighlightSpan> Collect(std::string_view code, std::string_view language,
                                       bool include_plain, std::size_t block_id = 0) const;
    // One Element per line of the block (vbox-able), colors from the theme.
    // Per the documented signature; built on HighlightCode.
    ftxui::Elements Highlight(std::string_view code, std::string_view language,
                              std::size_t block_id = 0) const override;

    // Whether `language` resolves to a registered grammar (aliases applied).
    bool Supports(std::string_view language) const;

    // CanonicalLanguage(language, this highlighter's own table).
    std::string CanonicalLanguageFor(std::string_view language) const override;

    // The Theme the highlighter holds is a REFERENCE whose contents may be
    // swapped in place at runtime (/theme); highlight_cache_ bakes the colors
    // into its cached Elements, so a swap must drop them or every cached block
    // keeps rendering the previous theme's palette. The parser cache is
    // theme-independent (parsers outlive color assignments) and survives.
    void OnThemeChanged();

    // The number of cached (language, code) entries right now. Render-thread-
    // only, same as Highlight() itself -- exists for regression tests that
    // assert the cache stays bounded across a streaming block's growth
    // instead of accumulating one near-duplicate prefix entry per tick.
    std::size_t CacheSizeForTesting() const { return highlight_cache_.size(); }

private:
    const Theme& theme_;
    const std::vector<LanguageDefinition>* languages_ = nullptr;  // never null

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
    mutable std::list<HighlightCacheEntry> highlight_cache_;   // MRU at front
};

// Normalizes a markdown fence label to a registered language's canonical
// name: a language's own name first (exactly, over every alias), then the
// aliases each declares ("c++", "py", "sh"); empty if none matches or there
// is no table.
std::string CanonicalLanguage(std::string_view info_string_language,
                              const std::vector<LanguageDefinition>* languages = nullptr);

}  // namespace minimal::client
