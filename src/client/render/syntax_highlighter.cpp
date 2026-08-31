#include "minimal/client/render/syntax_highlighter.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>

#include <tree_sitter/api.h>

extern "C" {
const TSLanguage* tree_sitter_cpp(void);
const TSLanguage* tree_sitter_python(void);
const TSLanguage* tree_sitter_javascript(void);
const TSLanguage* tree_sitter_typescript(void);
const TSLanguage* tree_sitter_bash(void);
const TSLanguage* tree_sitter_json(void);
const TSLanguage* tree_sitter_yaml(void);
const TSLanguage* tree_sitter_rust(void);
const TSLanguage* tree_sitter_go(void);
}

namespace minimal::client {

namespace {

// The per-language work, per ARCHITECTURE.md: one node-type -> category table.
// tree-sitter node types are stable within a grammar's major version. A name
// not listed is Plain, so a grammar update degrades coloring, never crashes.
// The rule type is the ABI-visible SyntaxRule (registries.hpp reuses it) so a
// registered grammar's table needs no translation.

struct Language {
    const char* name;
    const TSLanguage* (*language_fn)(void);
    std::vector<SyntaxRule> rules;
};

Language CppLanguage() {
    return {"cpp", tree_sitter_cpp,
            {
                // Storage classes and control flow the grammar types by name.
                {"namespace", HighlightCategory::Keyword},
                {"typename", HighlightCategory::Keyword},
                {"template", HighlightCategory::Keyword},
                {"virtual", HighlightCategory::Keyword},
                {"static_assert", HighlightCategory::Keyword},
                {"co_await", HighlightCategory::Keyword},
                {"co_yield", HighlightCategory::Keyword},
                {"co_return", HighlightCategory::Keyword},
                // Generic keyword node types.
                {"type_qualifier", HighlightCategory::Keyword},
                {"storage_class_specifier", HighlightCategory::Keyword},
                {"access_specifier", HighlightCategory::Keyword},
                {"if", HighlightCategory::Keyword},
                {"else", HighlightCategory::Keyword},
                {"for", HighlightCategory::Keyword},
                {"while", HighlightCategory::Keyword},
                {"do", HighlightCategory::Keyword},
                {"switch", HighlightCategory::Keyword},
                {"case", HighlightCategory::Keyword},
                {"default", HighlightCategory::Keyword},
                {"break", HighlightCategory::Keyword},
                {"continue", HighlightCategory::Keyword},
                {"return", HighlightCategory::Keyword},
                {"goto", HighlightCategory::Keyword},
                {"try", HighlightCategory::Keyword},
                {"catch", HighlightCategory::Keyword},
                {"throw", HighlightCategory::Keyword},
                {"new", HighlightCategory::Keyword},
                {"delete", HighlightCategory::Keyword},
                {"this", HighlightCategory::Keyword},
                {"concept", HighlightCategory::Keyword},
                {"requires", HighlightCategory::Keyword},
                {"true", HighlightCategory::Keyword},
                {"false", HighlightCategory::Keyword},
                {"nullptr", HighlightCategory::Keyword},
                {"primitive_type", HighlightCategory::Type},
                {"type_identifier", HighlightCategory::Type},
                {"sized_type_specifier", HighlightCategory::Type},
                {"auto", HighlightCategory::Type},
                {"string_literal", HighlightCategory::String},
                {"raw_string_literal", HighlightCategory::String},
                {"char_literal", HighlightCategory::String},
                {"string_content", HighlightCategory::String},
                {"escape_sequence", HighlightCategory::String},
                {"system_lib_string", HighlightCategory::String},
                {"comment", HighlightCategory::Comment},
                {"number_literal", HighlightCategory::Number},
                {"preproc_directive", HighlightCategory::Keyword},
                {"preproc_arg", HighlightCategory::String},
            }};
}

Language PythonLanguage() {
    return {"python", tree_sitter_python,
            {
                {"def", HighlightCategory::Keyword},
                {"class", HighlightCategory::Keyword},
                {"if", HighlightCategory::Keyword},
                {"elif", HighlightCategory::Keyword},
                {"else", HighlightCategory::Keyword},
                {"for", HighlightCategory::Keyword},
                {"while", HighlightCategory::Keyword},
                {"return", HighlightCategory::Keyword},
                {"yield", HighlightCategory::Keyword},
                {"import", HighlightCategory::Keyword},
                {"from", HighlightCategory::Keyword},
                {"as", HighlightCategory::Keyword},
                {"try", HighlightCategory::Keyword},
                {"except", HighlightCategory::Keyword},
                {"finally", HighlightCategory::Keyword},
                {"raise", HighlightCategory::Keyword},
                {"with", HighlightCategory::Keyword},
                {"lambda", HighlightCategory::Keyword},
                {"pass", HighlightCategory::Keyword},
                {"break", HighlightCategory::Keyword},
                {"continue", HighlightCategory::Keyword},
                {"global", HighlightCategory::Keyword},
                {"nonlocal", HighlightCategory::Keyword},
                {"assert", HighlightCategory::Keyword},
                {"del", HighlightCategory::Keyword},
                {"async", HighlightCategory::Keyword},
                {"await", HighlightCategory::Keyword},
                {"in", HighlightCategory::Keyword},
                {"is", HighlightCategory::Keyword},
                {"and", HighlightCategory::Keyword},
                {"or", HighlightCategory::Keyword},
                {"not", HighlightCategory::Keyword},
                {"match", HighlightCategory::Keyword},
                {"case", HighlightCategory::Keyword},
                {"none", HighlightCategory::Keyword},
                {"true", HighlightCategory::Keyword},
                {"false", HighlightCategory::Keyword},
                {"type", HighlightCategory::Type},
                {"string", HighlightCategory::String},
                {"string_content", HighlightCategory::String},
                {"escape_sequence", HighlightCategory::String},
                {"comment", HighlightCategory::Comment},
                {"integer", HighlightCategory::Number},
                {"float", HighlightCategory::Number},
            }};
}

Language JavascriptLanguage() {
    return {"javascript", tree_sitter_javascript,
            {
                {"function", HighlightCategory::Keyword},
                {"class", HighlightCategory::Keyword},
                {"extends", HighlightCategory::Keyword},
                {"if", HighlightCategory::Keyword},
                {"else", HighlightCategory::Keyword},
                {"for", HighlightCategory::Keyword},
                {"while", HighlightCategory::Keyword},
                {"do", HighlightCategory::Keyword},
                {"switch", HighlightCategory::Keyword},
                {"case", HighlightCategory::Keyword},
                {"default", HighlightCategory::Keyword},
                {"return", HighlightCategory::Keyword},
                {"yield", HighlightCategory::Keyword},
                {"break", HighlightCategory::Keyword},
                {"continue", HighlightCategory::Keyword},
                {"const", HighlightCategory::Keyword},
                {"let", HighlightCategory::Keyword},
                {"var", HighlightCategory::Keyword},
                {"new", HighlightCategory::Keyword},
                {"delete", HighlightCategory::Keyword},
                {"typeof", HighlightCategory::Keyword},
                {"instanceof", HighlightCategory::Keyword},
                {"in", HighlightCategory::Keyword},
                {"of", HighlightCategory::Keyword},
                {"try", HighlightCategory::Keyword},
                {"catch", HighlightCategory::Keyword},
                {"finally", HighlightCategory::Keyword},
                {"throw", HighlightCategory::Keyword},
                {"import", HighlightCategory::Keyword},
                {"export", HighlightCategory::Keyword},
                {"from", HighlightCategory::Keyword},
                {"async", HighlightCategory::Keyword},
                {"await", HighlightCategory::Keyword},
                {"static", HighlightCategory::Keyword},
                {"get", HighlightCategory::Keyword},
                {"set", HighlightCategory::Keyword},
                {"this", HighlightCategory::Keyword},
                {"super", HighlightCategory::Keyword},
                {"true", HighlightCategory::Keyword},
                {"false", HighlightCategory::Keyword},
                {"null", HighlightCategory::Keyword},
                {"undefined", HighlightCategory::Keyword},
                {"string", HighlightCategory::String},
                {"string_fragment", HighlightCategory::String},
                {"template_string", HighlightCategory::String},
                {"escape_sequence", HighlightCategory::String},
                {"regex", HighlightCategory::String},
                {"comment", HighlightCategory::Comment},
                {"html_comment", HighlightCategory::Comment},
                {"number", HighlightCategory::Number},
            }};
}

Language TypescriptLanguage() {
    Language ts = JavascriptLanguage();
    ts.name = "typescript";
    ts.language_fn = tree_sitter_typescript;
    ts.rules.push_back({"interface", HighlightCategory::Keyword});
    ts.rules.push_back({"type", HighlightCategory::Keyword});
    ts.rules.push_back({"enum", HighlightCategory::Keyword});
    ts.rules.push_back({"declare", HighlightCategory::Keyword});
    ts.rules.push_back({"namespace", HighlightCategory::Keyword});
    ts.rules.push_back({"implements", HighlightCategory::Keyword});
    ts.rules.push_back({"type_identifier", HighlightCategory::Type});
    ts.rules.push_back({"predefined_type", HighlightCategory::Type});
    return ts;
}

Language BashLanguage() {
    return {"bash", tree_sitter_bash,
            {
                {"if", HighlightCategory::Keyword},
                {"then", HighlightCategory::Keyword},
                {"elif", HighlightCategory::Keyword},
                {"else", HighlightCategory::Keyword},
                {"fi", HighlightCategory::Keyword},
                {"for", HighlightCategory::Keyword},
                {"in", HighlightCategory::Keyword},
                {"do", HighlightCategory::Keyword},
                {"done", HighlightCategory::Keyword},
                {"while", HighlightCategory::Keyword},
                {"until", HighlightCategory::Keyword},
                {"case", HighlightCategory::Keyword},
                {"esac", HighlightCategory::Keyword},
                {"function", HighlightCategory::Keyword},
                {"string", HighlightCategory::String},
                {"string_content", HighlightCategory::String},
                {"raw_string", HighlightCategory::String},
                {"ansi_c_string", HighlightCategory::String},
                {"heredoc_body", HighlightCategory::String},
                {"comment", HighlightCategory::Comment},
                {"number", HighlightCategory::Number},
            }};
}

Language JsonLanguage() {
    return {"json", tree_sitter_json,
            {
                {"string", HighlightCategory::String},
                {"string_content", HighlightCategory::String},
                {"escape_sequence", HighlightCategory::String},
                {"comment", HighlightCategory::Comment},
                {"number", HighlightCategory::Number},
                {"true", HighlightCategory::Keyword},
                {"false", HighlightCategory::Keyword},
                {"null", HighlightCategory::Keyword},
            }};
}

Language YamlLanguage() {
    return {"yaml", tree_sitter_yaml,
            {
                {"string_scalar", HighlightCategory::String},
                {"single_quote_scalar", HighlightCategory::String},
                {"double_quote_scalar", HighlightCategory::String},
                {"block_scalar", HighlightCategory::String},
                {"escape_sequence", HighlightCategory::String},
                {"comment", HighlightCategory::Comment},
                {"integer_scalar", HighlightCategory::Number},
                {"float_scalar", HighlightCategory::Number},
                {"boolean_scalar", HighlightCategory::Keyword},
                {"null_scalar", HighlightCategory::Keyword},
                {"anchor_name", HighlightCategory::Type},
                {"alias_name", HighlightCategory::Type},
                {"tag", HighlightCategory::Type},
            }};
}

Language RustLanguage() {
    return {"rust", tree_sitter_rust,
            {
                {"fn", HighlightCategory::Keyword},
                {"let", HighlightCategory::Keyword},
                {"mut", HighlightCategory::Keyword},
                {"const", HighlightCategory::Keyword},
                {"static", HighlightCategory::Keyword},
                {"if", HighlightCategory::Keyword},
                {"else", HighlightCategory::Keyword},
                {"match", HighlightCategory::Keyword},
                {"for", HighlightCategory::Keyword},
                {"while", HighlightCategory::Keyword},
                {"loop", HighlightCategory::Keyword},
                {"in", HighlightCategory::Keyword},
                {"return", HighlightCategory::Keyword},
                {"break", HighlightCategory::Keyword},
                {"continue", HighlightCategory::Keyword},
                {"struct", HighlightCategory::Keyword},
                {"enum", HighlightCategory::Keyword},
                {"impl", HighlightCategory::Keyword},
                {"trait", HighlightCategory::Keyword},
                {"type", HighlightCategory::Keyword},
                {"where", HighlightCategory::Keyword},
                {"use", HighlightCategory::Keyword},
                {"mod", HighlightCategory::Keyword},
                {"pub", HighlightCategory::Keyword},
                {"crate", HighlightCategory::Keyword},
                {"self", HighlightCategory::Keyword},
                {"super", HighlightCategory::Keyword},
                {"async", HighlightCategory::Keyword},
                {"await", HighlightCategory::Keyword},
                {"move", HighlightCategory::Keyword},
                {"ref", HighlightCategory::Keyword},
                {"unsafe", HighlightCategory::Keyword},
                {"extern", HighlightCategory::Keyword},
                {"dyn", HighlightCategory::Keyword},
                {"as", HighlightCategory::Keyword},
                {"true", HighlightCategory::Keyword},
                {"false", HighlightCategory::Keyword},
                {"string_literal", HighlightCategory::String},
                {"raw_string_literal", HighlightCategory::String},
                {"char_literal", HighlightCategory::String},
                {"string_content", HighlightCategory::String},
                {"escape_sequence", HighlightCategory::String},
                {"line_comment", HighlightCategory::Comment},
                {"block_comment", HighlightCategory::Comment},
                {"integer_literal", HighlightCategory::Number},
                {"float_literal", HighlightCategory::Number},
                {"primitive_type", HighlightCategory::Type},
                {"type_identifier", HighlightCategory::Type},
            }};
}

Language GoLanguage() {
    return {"go", tree_sitter_go,
            {
                {"func", HighlightCategory::Keyword},
                {"var", HighlightCategory::Keyword},
                {"const", HighlightCategory::Keyword},
                {"type", HighlightCategory::Keyword},
                {"struct", HighlightCategory::Keyword},
                {"interface", HighlightCategory::Keyword},
                {"map", HighlightCategory::Keyword},
                {"chan", HighlightCategory::Keyword},
                {"if", HighlightCategory::Keyword},
                {"else", HighlightCategory::Keyword},
                {"for", HighlightCategory::Keyword},
                {"range", HighlightCategory::Keyword},
                {"switch", HighlightCategory::Keyword},
                {"select", HighlightCategory::Keyword},
                {"case", HighlightCategory::Keyword},
                {"default", HighlightCategory::Keyword},
                {"return", HighlightCategory::Keyword},
                {"break", HighlightCategory::Keyword},
                {"continue", HighlightCategory::Keyword},
                {"goto", HighlightCategory::Keyword},
                {"defer", HighlightCategory::Keyword},
                {"go", HighlightCategory::Keyword},
                {"package", HighlightCategory::Keyword},
                {"import", HighlightCategory::Keyword},
                {"fallthrough", HighlightCategory::Keyword},
                {"true", HighlightCategory::Keyword},
                {"false", HighlightCategory::Keyword},
                {"nil", HighlightCategory::Keyword},
                {"raw_string_literal", HighlightCategory::String},
                {"interpreted_string_literal", HighlightCategory::String},
                {"rune_literal", HighlightCategory::String},
                {"escape_sequence", HighlightCategory::String},
                {"comment", HighlightCategory::Comment},
                {"int_literal", HighlightCategory::Number},
                {"float_literal", HighlightCategory::Number},
                {"imaginary_literal", HighlightCategory::Number},
                {"type_identifier", HighlightCategory::Type},
            }};
}

const std::array<Language, 9>& VendoredTable() {
    static const std::array<Language, 9> kLanguages = {
        CppLanguage(),   PythonLanguage(), JavascriptLanguage(), TypescriptLanguage(),
        BashLanguage(),  JsonLanguage(),   YamlLanguage(),       RustLanguage(),
        GoLanguage(),
    };
    return kLanguages;
}

const Language& FindLanguage(std::string_view language) {
    static const Language kUnsupported{"", nullptr, {}};
    for (const Language& entry : VendoredTable()) {
        if (language == entry.name) return entry;
    }
    return kUnsupported;
}

HighlightCategory Categorize(const Language& language, const char* node_type) {
    for (const SyntaxRule& rule : language.rules) {
        if (node_type == rule.node_type) return rule.category;
    }
    return HighlightCategory::Plain;
}

const char* ThemeRoleFor(HighlightCategory category) {
    switch (category) {
        case HighlightCategory::Keyword:
            return "syntax_keyword";
        case HighlightCategory::String:
            return "syntax_string";
        case HighlightCategory::Comment:
            return "syntax_comment";
        case HighlightCategory::Number:
            return "syntax_number";
        case HighlightCategory::Function:
            return "syntax_function";
        case HighlightCategory::Type:
            return "syntax_type";
        case HighlightCategory::Operator:
            return "syntax_operator";
        case HighlightCategory::Plain:
            return "syntax_plain";
    }
    return "syntax_plain";
}

// Plain unless split by a later (deeper) node's category: traversal is
// preorder, so a parent claims its whole range first and children re-split it.
// Keywords in tree-sitter grammars are usually ANONYMOUS nodes ("return", "if")
// -- string literals in the grammar -- so anonymous nodes are checked against
// the rule table too; only those that match (or named nodes generally) append
// a span, or every "(" and ";" would add a boundary for no visible change.
void AppendSpan(std::vector<HighlightSpan>& spans, HighlightCategory category,
                uint32_t start, uint32_t end) {
    if (start >= end) return;
    spans.push_back({category, start, end});
}

}  // namespace

std::vector<LanguageDefinition> VendoredLanguages() {
    std::vector<LanguageDefinition> out;
    out.reserve(VendoredTable().size());
    for (const Language& entry : VendoredTable()) {
        out.push_back(LanguageDefinition{entry.name, entry.language_fn, entry.rules});
    }
    return out;
}

std::string CanonicalLanguage(std::string_view language,
                              const std::vector<LanguageDefinition>* languages) {
    struct Alias {
        const char* alias;
        const char* canonical;
    };
    static constexpr Alias kAliases[] = {
        {"c++", "cpp"}, {"cc", "cpp"}, {"cxx", "cpp"}, {"h", "cpp"},   {"hpp", "cpp"},
        {"py", "python"}, {"python3", "python"},
        {"js", "javascript"}, {"jsx", "javascript"}, {"node", "javascript"},
        {"ts", "typescript"}, {"tsx", "typescript"},
        {"sh", "bash"}, {"shell", "bash"}, {"zsh", "bash"},
        {"jsonc", "json"}, {"json5", "json"},
        {"yml", "yaml"},
        {"rs", "rust"},
        {"golang", "go"},
    };
    std::string lower(language);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const Alias& alias : kAliases) {
        if (lower == alias.alias) return alias.canonical;
    }
    if (languages != nullptr) {
        // A registered language resolves EXACTLY -- no aliases unless it
        // supplies them; a plugin grammar's key is its own.
        for (const LanguageDefinition& entry : *languages) {
            if (lower == entry.name) return lower;
        }
        return "";
    }
    if (FindLanguage(lower).language_fn != nullptr) return lower;
    return "";
}

bool SyntaxHighlighter::Supports(std::string_view language) const {
    return !CanonicalLanguage(language, languages_).empty();
}

SyntaxHighlighter::SyntaxHighlighter(const Theme& theme)
    : theme_(theme), languages_(nullptr) {}

SyntaxHighlighter::SyntaxHighlighter(const Theme& theme,
                                     const std::vector<LanguageDefinition>& languages)
    : theme_(theme), languages_(&languages) {}

std::vector<HighlightSpan> SyntaxHighlighter::HighlightCode(std::string_view code,
                                                            std::string_view language) const {
    // Two lookup paths, one traversal: the vendored table (default) and the
    // injected table the host assembles from the grammar registry. The
    // categorizer differs; everything else is shared.
    const Language* vendored = nullptr;
    const LanguageDefinition* injected = nullptr;
    if (languages_ != nullptr) {
        const std::string canonical = CanonicalLanguage(language, languages_);
        for (const LanguageDefinition& entry : *languages_) {
            if (canonical == entry.name) {
                injected = &entry;
                break;
            }
        }
        if (injected == nullptr) {
            return {{HighlightCategory::Plain, 0, static_cast<uint32_t>(code.size())}};
        }
    } else {
        vendored = &FindLanguage(CanonicalLanguage(language));
        if (vendored->language_fn == nullptr) {
            return {{HighlightCategory::Plain, 0, static_cast<uint32_t>(code.size())}};
        }
    }
    const TSLanguage* (*language_fn)(void) =
        injected != nullptr ? injected->language_fn : vendored->language_fn;
    const auto categorize = [&](const char* node_type) -> HighlightCategory {
        if (injected != nullptr) {
            for (const SyntaxRule& rule : injected->rules) {
                if (node_type == rule.node_type) return rule.category;
            }
            return HighlightCategory::Plain;
        }
        return Categorize(*vendored, node_type);
    };

    TSParser* parser = ts_parser_new();
    if (!ts_parser_set_language(parser, language_fn())) {
        ts_parser_delete(parser);
        return {{HighlightCategory::Plain, 0, static_cast<uint32_t>(code.size())}};
    }
    TSTree* tree = ts_parser_parse_string(parser, nullptr, code.data(),
                                          static_cast<uint32_t>(code.size()));
    ts_parser_delete(parser);
    if (tree == nullptr) {
        return {{HighlightCategory::Plain, 0, static_cast<uint32_t>(code.size())}};
    }

    std::vector<HighlightSpan> spans;
    TSTreeCursor cursor = ts_tree_cursor_new(ts_tree_root_node(tree));
    bool descend = true;
    while (true) {
        if (descend) {
            const TSNode node = ts_tree_cursor_current_node(&cursor);
            const HighlightCategory category = categorize(ts_node_type(node));
            if (ts_node_is_named(node) || category != HighlightCategory::Plain) {
                AppendSpan(spans, category, ts_node_start_byte(node), ts_node_end_byte(node));
            }
        }
        if (descend && ts_tree_cursor_goto_first_child(&cursor)) {
            descend = true;
        } else if (ts_tree_cursor_goto_next_sibling(&cursor)) {
            descend = true;
        } else {
            // Climb until a parent has an unvisited sibling, or the root.
            bool moved = false;
            while (ts_tree_cursor_goto_parent(&cursor)) {
                if (ts_tree_cursor_goto_next_sibling(&cursor)) {
                    moved = true;
                    descend = true;
                    break;
                }
            }
            if (!moved) break;
        }
    }
    ts_tree_cursor_delete(&cursor);
    ts_tree_delete(tree);
    return spans;
}

ftxui::Elements SyntaxHighlighter::Highlight(std::string_view code,
                                             std::string_view language) const {
    const std::vector<HighlightSpan> spans = HighlightCode(code, language);

    ftxui::Elements lines;
    ftxui::Elements current_line;
    uint32_t pos = 0;

    const auto flush_line = [&]() {
        lines.push_back(current_line.empty() ? ftxui::text("") : ftxui::hbox(current_line));
        current_line.clear();
    };
    const auto emit_text = [&](std::string_view text, HighlightCategory category) {
        ftxui::Element element = ftxui::text(std::string(text));
        const ftxui::Color c = theme_.ColorFor(ThemeRoleFor(category));
        if (category != HighlightCategory::Plain && c != ftxui::Color::Default) {
            element = element | ftxui::color(c);
        }
        current_line.push_back(element);
    };

    // Emits text between [cursor, end), splitting at newlines. Positions are
    // size_t until the newline check: casting find()'s npos to uint32_t first
    // would make the npos comparison never fire.
    const auto emit_range = [&](uint32_t begin, uint32_t end, HighlightCategory category) {
        size_t cursor = begin;
        while (cursor < end) {
            const size_t next = code.find('\n', cursor);
            if (next == std::string_view::npos || next >= end) {
                emit_text(code.substr(cursor, end - cursor), category);
                return;
            }
            emit_text(code.substr(cursor, next - cursor), category);
            flush_line();
            cursor = next + 1;
        }
    };

    for (const HighlightSpan& span : spans) {
        // Plain spans are informational defaults, not claims: a Plain parent
        // (translation_unit, ...) covers the whole input, and treating it as a
        // claim would clip every categorized child inside it. Gaps are already
        // rendered Plain, so skipping Plain spans loses nothing.
        if (span.category == HighlightCategory::Plain) continue;
        const uint32_t begin = std::max(span.start_byte, pos);
        emit_range(pos, begin, HighlightCategory::Plain);
        emit_range(begin, span.end_byte, span.category);
        pos = std::max(pos, span.end_byte);
    }
    // Trailing text after the last span.
    emit_range(pos, static_cast<uint32_t>(code.size()), HighlightCategory::Plain);
    flush_line();
    return lines;
}

}  // namespace minimal::client
