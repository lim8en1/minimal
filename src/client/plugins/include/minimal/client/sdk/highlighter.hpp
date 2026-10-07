#pragma once

// The code-highlighting surface plugins see: the categories a grammar maps
// node types to, a grammar's own description, and the host's highlighter
// (ClientPluginRegistrationContext::highlighter). The tree-sitter highlighter
// itself is core's (render/syntax_highlighter.hpp). TSLanguage is only named
// here: a grammar plugin includes tree-sitter for its own parser.

#include <span>
#include <string>
#include <string_view>

#include <ftxui/dom/elements.hpp>

extern "C" {
typedef struct TSLanguage TSLanguage;
}

namespace minimal::client {

enum class HighlightCategory { Keyword, String, Comment, Number, Function, Type, Operator, Plain };

// One rule: a tree-sitter node type and the category it maps to. Matches
// ANONYMOUS node types too -- tree-sitter keywords are anonymous nodes, which
// the rule tables exploit deliberately.
struct SyntaxRule {
    std::string_view node_type;
    HighlightCategory category;
};

// A function name/call-target position, identified by (parent node type,
// field name) rather than by node type alone: tree-sitter grammars use the
// SAME node type ("identifier") for a function name as for any other
// identifier, so the flat SyntaxRule table (type -> category) can't tell
// `foo` in `foo()` from `foo` in `x = foo`. The traversal already tracks the
// current node's field name and its parent's type, so matching is exact.
struct FunctionFieldRule {
    std::string_view parent_type;
    std::string_view field_name;
};

// One language: canonical key, its parser, its node-type table, its
// function-name positions and the fence labels that also mean it. The
// grammar registry hands these to the highlighter (the bundled
// syntax-grammars plugin registers the usual set).
using LanguageFnPtr = const TSLanguage* (*)(void);

struct LanguageDefinition {
    std::string_view name;
    LanguageFnPtr language_fn;
    std::span<const SyntaxRule> rules;
    std::span<const FunctionFieldRule> function_fields = {};
    std::span<const std::string_view> aliases = {};
};

// tree-sitter core plus a curated, vendored grammar set. A regex tokenizer was
// considered and rejected: nested/context-free constructs (Rust nested block
// comments, C++ raw strings, JS template literals) are exactly where correctness
// matters and exactly where regex fails.
//
// The highlighter never hardcodes a color; the theme supplies one per category.
// Code highlighting as a markdown engine (or any renderer) uses it: the
// host's highlighter, over every registered grammar, handed to plugins
// through ClientPluginRegistrationContext::highlighter.
class CodeHighlighter {
public:
    virtual ~CodeHighlighter() = default;
    // `code` as highlighted lines, colored by the current theme. `block_id`
    // (non-zero) names a growing streaming block, so its parse is
    // incremental. An unknown language renders plain.
    virtual ftxui::Elements Highlight(std::string_view code, std::string_view language,
                                      std::size_t block_id = 0) const = 0;
    // A fence label's canonical language ("py" -> "python"); "" when no
    // registered grammar knows it.
    virtual std::string CanonicalLanguageFor(std::string_view language) const = 0;
};

}  // namespace minimal::client
