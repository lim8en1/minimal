#include "minimal/client/render/syntax_highlighter.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

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

// The TSPoint (row, column) reached after scanning `text`, starting from
// `start`. Used to build the TSInputEdit a streaming block's incremental
// reparse needs -- only over the APPENDED suffix (see ParserCache::AdoptTree
// below), not the whole growing block, so this stays cheap per debounce tick.
TSPoint AdvancePoint(TSPoint start, std::string_view text) {
    TSPoint point = start;
    for (const char ch : text) {
        if (ch == '\n') {
            ++point.row;
            point.column = 0;
        } else {
            ++point.column;
        }
    }
    return point;
}

// The per-language work, per ARCHITECTURE.md: one node-type -> category table.
// tree-sitter node types are stable within a grammar's major version. A name
// not listed is Plain, so a grammar update degrades coloring, never crashes.
// The rule type is the ABI-visible SyntaxRule (registries.hpp reuses it) so a
// registered grammar's table needs no translation.

// A function name/call-target position, identified by (parent node type,
// field name) rather than by node type alone: tree-sitter grammars use the
// SAME node type ("identifier") for a function name as for any other
// identifier, so the flat SyntaxRule table (type -> category) can't tell
// `foo` in `foo()` from `foo` in `x = foo`. The cursor traversal in Collect()
// already tracks the current node's field name and its parent's type at
// negligible extra cost, so matching here is exact -- no false positives on
// ordinary identifiers. Field names verified against this build's vendored
// grammar sources (_deps/tsgrammar_*-src/**/node-types.json).
struct FunctionFieldRule {
    std::string_view parent_type;
    std::string_view field_name;
};

struct Language {
    const char* name;
    const TSLanguage* (*language_fn)(void);
    std::vector<SyntaxRule> rules;
    std::vector<FunctionFieldRule> function_fields;
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

                {"+", HighlightCategory::Operator},   {"-", HighlightCategory::Operator},
                {"*", HighlightCategory::Operator},   {"/", HighlightCategory::Operator},
                {"%", HighlightCategory::Operator},   {"=", HighlightCategory::Operator},
                {"==", HighlightCategory::Operator},  {"!=", HighlightCategory::Operator},
                {"<", HighlightCategory::Operator},   {">", HighlightCategory::Operator},
                {"<=", HighlightCategory::Operator},  {">=", HighlightCategory::Operator},
                {"&&", HighlightCategory::Operator},  {"||", HighlightCategory::Operator},
                {"!", HighlightCategory::Operator},   {"&", HighlightCategory::Operator},
                {"|", HighlightCategory::Operator},   {"^", HighlightCategory::Operator},
                {"~", HighlightCategory::Operator},   {"<<", HighlightCategory::Operator},
                {">>", HighlightCategory::Operator},  {"->", HighlightCategory::Operator},
                {"++", HighlightCategory::Operator},  {"--", HighlightCategory::Operator},
                {"?", HighlightCategory::Operator},   {":", HighlightCategory::Operator},
                {".", HighlightCategory::Operator},   {"::", HighlightCategory::Operator},
            },
            {
                {"call_expression", "function"},
                {"function_declarator", "declarator"},
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

                {"+", HighlightCategory::Operator},   {"-", HighlightCategory::Operator},
                {"*", HighlightCategory::Operator},   {"/", HighlightCategory::Operator},
                {"%", HighlightCategory::Operator},   {"=", HighlightCategory::Operator},
                {"==", HighlightCategory::Operator},  {"!=", HighlightCategory::Operator},
                {"<", HighlightCategory::Operator},   {">", HighlightCategory::Operator},
                {"<=", HighlightCategory::Operator},  {">=", HighlightCategory::Operator},
                {"and", HighlightCategory::Operator}, {"or", HighlightCategory::Operator},
                {"not", HighlightCategory::Operator}, {"in", HighlightCategory::Operator},
                {"is", HighlightCategory::Operator},
            },
            {
                {"call", "function"},
                {"function_definition", "name"},
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

                {"+", HighlightCategory::Operator},   {"-", HighlightCategory::Operator},
                {"*", HighlightCategory::Operator},   {"/", HighlightCategory::Operator},
                {"%", HighlightCategory::Operator},   {"=", HighlightCategory::Operator},
                {"==", HighlightCategory::Operator},  {"===", HighlightCategory::Operator},
                {"!=", HighlightCategory::Operator},  {"!==", HighlightCategory::Operator},
                {"<", HighlightCategory::Operator},   {">", HighlightCategory::Operator},
                {"<=", HighlightCategory::Operator},  {">=", HighlightCategory::Operator},
                {"&&", HighlightCategory::Operator},  {"||", HighlightCategory::Operator},
                {"!", HighlightCategory::Operator},   {"?", HighlightCategory::Operator},
                {":", HighlightCategory::Operator},   {".", HighlightCategory::Operator},
                {"=>", HighlightCategory::Operator},
            },
            {
                {"call_expression", "function"},
                {"function_declaration", "name"},
                {"method_definition", "name"},
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
    // call_expression/function_declaration/method_definition field names are
    // identical to javascript's in this vendored grammar; method_signature
    // (interface/ambient declarations, no body) is TS-only.
    ts.function_fields.push_back({"method_signature", "name"});
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

                {"+", HighlightCategory::Operator},   {"-", HighlightCategory::Operator},
                {"*", HighlightCategory::Operator},   {"/", HighlightCategory::Operator},
                {"%", HighlightCategory::Operator},   {"=", HighlightCategory::Operator},
                {"==", HighlightCategory::Operator},  {"!=", HighlightCategory::Operator},
                {"<", HighlightCategory::Operator},   {">", HighlightCategory::Operator},
                {"<=", HighlightCategory::Operator},  {">=", HighlightCategory::Operator},
                {"&&", HighlightCategory::Operator},  {"||", HighlightCategory::Operator},
                {"!", HighlightCategory::Operator},   {":", HighlightCategory::Operator},
                {".", HighlightCategory::Operator},
                {"|", HighlightCategory::Operator},   {"&", HighlightCategory::Operator},
                {">>", HighlightCategory::Operator},  {"<<", HighlightCategory::Operator},
            },
            {}};
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
            },
            {}};
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
            },
            {}};
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

                {"+", HighlightCategory::Operator},   {"-", HighlightCategory::Operator},
                {"*", HighlightCategory::Operator},   {"/", HighlightCategory::Operator},
                {"%", HighlightCategory::Operator},   {"=", HighlightCategory::Operator},
                {"==", HighlightCategory::Operator},  {"!=", HighlightCategory::Operator},
                {"<", HighlightCategory::Operator},   {">", HighlightCategory::Operator},
                {"<=", HighlightCategory::Operator},  {">=", HighlightCategory::Operator},
                {"&&", HighlightCategory::Operator},  {"||", HighlightCategory::Operator},
                {"!", HighlightCategory::Operator},   {"&", HighlightCategory::Operator},
                {"|", HighlightCategory::Operator},   {"^", HighlightCategory::Operator},
                {"->", HighlightCategory::Operator},  {"=>", HighlightCategory::Operator},
                {"::", HighlightCategory::Operator},  {".", HighlightCategory::Operator},
                {"?", HighlightCategory::Operator},
            },
            {
                {"call_expression", "function"},
                {"function_item", "name"},
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

                {"+", HighlightCategory::Operator},   {"-", HighlightCategory::Operator},
                {"*", HighlightCategory::Operator},   {"/", HighlightCategory::Operator},
                {"%", HighlightCategory::Operator},   {"=", HighlightCategory::Operator},
                {"==", HighlightCategory::Operator},  {"!=", HighlightCategory::Operator},
                {"<", HighlightCategory::Operator},   {">", HighlightCategory::Operator},
                {"<=", HighlightCategory::Operator},  {">=", HighlightCategory::Operator},
                {"&&", HighlightCategory::Operator},  {"||", HighlightCategory::Operator},
                {"!", HighlightCategory::Operator},   {"&", HighlightCategory::Operator},
                {"|", HighlightCategory::Operator},   {"^", HighlightCategory::Operator},
                {"<-", HighlightCategory::Operator},  {":=", HighlightCategory::Operator},
                {".", HighlightCategory::Operator},
            },
            {
                {"call_expression", "function"},
                {"function_declaration", "name"},
                {"method_declaration", "name"},
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
    static const Language kUnsupported{"", nullptr, {}, {}};
    for (const Language& entry : VendoredTable()) {
        if (language == entry.name) return entry;
    }
    return kUnsupported;
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

// Spans come out in preorder; ApplySpanOverrides flattens them so a DEEPER
// node wins over its ancestors (see it above Highlight).
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
        {"c", "cpp"},   {"hh", "cpp"}, {"hxx", "cpp"},
        {"py", "python"}, {"python3", "python"},
        {"js", "javascript"}, {"jsx", "javascript"}, {"node", "javascript"},
        {"mjs", "javascript"}, {"cjs", "javascript"},
        {"ts", "typescript"}, {"tsx", "typescript"},
        {"sh", "bash"}, {"shell", "bash"}, {"zsh", "bash"},
        {"jsonc", "json"}, {"json5", "json"},
        {"yml", "yaml"},
        {"rs", "rust"},
        {"golang", "go"},
    };
    // Reused buffer: this runs per code block per render; the first draft
    // allocated a fresh std::string every call.
    thread_local std::string lower;
    lower.assign(language);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (languages != nullptr && !languages->empty()) {
        // A plugin-registered language resolves EXACTLY and FIRST: a plugin
        // grammar's key is its own. (The alias table used to run first, so a
        // plugin registering "shell", "node" or "cc" was silently shadowed
        // into a vendored name that then resolved to nothing.) Anything NOT
        // registered falls through to the vendored aliases below, so the
        // default highlighting for "js" etc. is unchanged.
        const auto iequals = [](std::string_view a, std::string_view b) {
            return a.size() == b.size() &&
                   std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                       return std::tolower(static_cast<unsigned char>(x)) ==
                              std::tolower(static_cast<unsigned char>(y));
                   });
        };
        for (const LanguageDefinition& entry : *languages) {
            if (iequals(lower, entry.name)) return lower;
        }
    }
    for (const Alias& alias : kAliases) {
        if (lower == alias.alias) return alias.canonical;
    }
    if (FindLanguage(lower).language_fn != nullptr) return lower;
    return "";
}

bool SyntaxHighlighter::Supports(std::string_view language) const {
    return !CanonicalLanguage(language, languages_).empty();
}

std::string SyntaxHighlighter::CanonicalLanguageFor(std::string_view language) const {
    return CanonicalLanguage(language, languages_);
}

SyntaxHighlighter::SyntaxHighlighter(const Theme& theme)
    : theme_(theme), languages_(nullptr), parser_cache_(std::make_unique<ParserCache>()) {}

SyntaxHighlighter::SyntaxHighlighter(const Theme& theme,
                                     const std::vector<LanguageDefinition>& languages)
    : theme_(theme), languages_(&languages), parser_cache_(std::make_unique<ParserCache>()) {}

SyntaxHighlighter::~SyntaxHighlighter() = default;

void SyntaxHighlighter::OnThemeChanged() { highlight_cache_.clear(); }


// One TSParser per language, kept alive for the highlighter's lifetime (track
// R3): the first draft ran ts_parser_new/set_language/delete PER HIGHLIGHT
// CALL, which the streaming path pays every 120 ms tick for every code block.
// A nullptr value negative-caches a language whose parser failed to load.
class SyntaxHighlighter::ParserCache {
public:
    ~ParserCache() {
        for (auto& [name, parser] : parsers_) {
            if (parser != nullptr) ts_parser_delete(parser);
        }
        for (auto& [name, tree] : trees_) {
            if (tree != nullptr) ts_tree_delete(tree);
        }
    }

    TSParser* Get(const std::string& canonical, LanguageFnPtr language_fn, bool& ok) {
        const auto it = parsers_.find(canonical);
        if (it != parsers_.end()) {
            ok = it->second != nullptr;
            return it->second;
        }
        TSParser* parser = ts_parser_new();
        if (!ts_parser_set_language(parser, language_fn())) {
            ts_parser_delete(parser);
            parser = nullptr;
        }
        parsers_.emplace(canonical, parser);
        ok = parser != nullptr;
        return parser;
    }

    // The previous parse's tree for `canonical`, or nullptr. Incremental
    // parsing hint for the streaming path: a code block still growing is
    // re-highlighted every debounce tick, and each reparse used to start from
    // scratch. Tree-sitter requires the base tree to have been through
    // ts_tree_edit() describing exactly how its source changed -- passing an
    // unedited tree from DIFFERENT source (a different code block, or this
    // same block's text changed some other way) makes it reuse old nodes by
    // byte position against text that never produced them, which is silently
    // WRONG, not just less-reused. Collect() only offers this tree back to
    // the parser after confirming (via LastSource) it is a valid base for an
    // append and editing it accordingly; everything else gets a full reparse
    // (nullptr base), same as a cache miss.
    TSTree* LastTree(const std::string& canonical) {
        const auto it = trees_.find(canonical);
        return it != trees_.end() ? it->second : nullptr;
    }

    // The exact source string `LastTree(canonical)` was parsed from, and the
    // TSPoint reached at its end -- both needed to build the TSInputEdit for
    // an append, and both kept here (not recomputed) so a growing block's
    // per-tick cost stays proportional to what changed, not to the whole
    // block.
    const std::string* LastSource(const std::string& canonical) const {
        const auto it = last_code_.find(canonical);
        return it != last_code_.end() ? &it->second : nullptr;
    }
    TSPoint LastEndPoint(const std::string& canonical) const {
        const auto it = last_point_.find(canonical);
        return it != last_point_.end() ? it->second : TSPoint{0, 0};
    }

    // Takes ownership of `tree` for `canonical`, releasing the tree it
    // replaces, and remembers the source text and end point it was parsed
    // from (LastSource/LastEndPoint above). Only called with a tree from this
    // language's own parser (tree-sitter requires the base tree and the
    // parser to match).
    void AdoptTree(const std::string& canonical, TSTree* tree, std::string code,
                   TSPoint end_point) {
        const auto it = trees_.find(canonical);
        if (it != trees_.end() && it->second != nullptr) ts_tree_delete(it->second);
        trees_[canonical] = tree;
        last_code_[canonical] = std::move(code);
        last_point_[canonical] = end_point;
    }

    // Drops `canonical`'s cached tree/source/point entirely (see Collect()'s
    // parse-failure-after-edit path): forces the next call for this language
    // into a full reparse rather than risk an inconsistent incremental base.
    void Invalidate(const std::string& canonical) {
        if (auto it = trees_.find(canonical); it != trees_.end()) {
            if (it->second != nullptr) ts_tree_delete(it->second);
            trees_.erase(it);
        }
        last_code_.erase(canonical);
        last_point_.erase(canonical);
        last_spans_.erase(canonical);
    }

    // The full span list Collect() returned last time for `canonical` (this
    // tree cache's key, which already folds in the caller's block_id -- see
    // Collect()). Paired with LastTree/LastSource to let an incremental parse
    // reuse the spans covering the region ts_tree_get_changed_ranges proves
    // is untouched, instead of re-walking the whole tree every debounce tick.
    const std::vector<HighlightSpan>* LastSpans(const std::string& canonical) const {
        const auto it = last_spans_.find(canonical);
        return it != last_spans_.end() ? &it->second : nullptr;
    }
    void AdoptSpans(const std::string& canonical, std::vector<HighlightSpan> spans) {
        last_spans_[canonical] = std::move(spans);
    }

    // Node-type -> category, hashed once per language (the first draft walked
    // every rule per node with content-comparing string_view equality). Keys
    // are views into stable storage: tree-sitter's static type strings and the
    // rule tables' static/span-backed names.
    using RuleTable = std::unordered_map<std::string_view, HighlightCategory>;

    const RuleTable& TableFor(const std::string& canonical, LanguageFnPtr language_fn,
                              const Language* vendored, const LanguageDefinition* injected) {
        const auto it = tables_.find(canonical);
        if (it != tables_.end()) return it->second;
        RuleTable table;
        if (injected != nullptr) {
            for (const SyntaxRule& rule : injected->rules) {
                table.emplace(rule.node_type, rule.category);
            }
        } else {
            for (const SyntaxRule& rule : vendored->rules) {
                table.emplace(rule.node_type, rule.category);
            }
        }
        (void)language_fn;
        return tables_.emplace(canonical, std::move(table)).first->second;
    }

private:
    std::map<std::string, TSParser*> parsers_;
    std::unordered_map<std::string, RuleTable> tables_;
    // One kept-alive tree per language, the base of the next incremental
    // parse (see LastTree/AdoptTree above). Bounded by the language count.
    std::unordered_map<std::string, TSTree*> trees_;
    // The exact source `trees_[canonical]` was parsed from, and the point
    // reached at its end -- see LastSource/LastEndPoint. Bounded the same way
    // trees_ is (one entry per language); each entry is one code block's
    // worth of text, freed the moment a different or completed block reuses
    // the slot.
    std::unordered_map<std::string, std::string> last_code_;
    std::unordered_map<std::string, TSPoint> last_point_;
    // See LastSpans/AdoptSpans -- one entry per tree cache slot, same lifetime
    // as trees_/last_code_/last_point_ (cleared together by Invalidate).
    std::unordered_map<std::string, std::vector<HighlightSpan>> last_spans_;
};

std::vector<HighlightSpan> SyntaxHighlighter::HighlightCode(std::string_view code,
                                                            std::string_view language,
                                                            std::size_t block_id) const {
    return Collect(code, language, /*include_plain=*/true, block_id);
}

std::vector<HighlightSpan> SyntaxHighlighter::Collect(std::string_view code,
                                                      std::string_view language,
                                                      bool include_plain,
                                                      std::size_t block_id) const {
    // Two lookup paths, one traversal: the vendored table (default) and the
    // injected table the host assembles from the grammar registry. The
    // categorizer differs; everything else is shared.
    const Language* vendored = nullptr;
    const LanguageDefinition* injected = nullptr;
    std::string canonical;
    if (languages_ != nullptr) {
        canonical = CanonicalLanguage(language, languages_);
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
        canonical = CanonicalLanguage(language);
        vendored = &FindLanguage(canonical);
        if (vendored->language_fn == nullptr) {
            return {{HighlightCategory::Plain, 0, static_cast<uint32_t>(code.size())}};
        }
    }
    const TSLanguage* (*language_fn)(void) =
        injected != nullptr ? injected->language_fn : vendored->language_fn;

    bool parser_ok = false;
    TSParser* parser = parser_cache_->Get(canonical, language_fn, parser_ok);
    if (!parser_ok || parser == nullptr) {
        return {{HighlightCategory::Plain, 0, static_cast<uint32_t>(code.size())}};
    }
    const auto& rule_table =
        parser_cache_->TableFor(canonical, language_fn, vendored, injected);
    const auto categorize = [&rule_table](const char* node_type) -> HighlightCategory {
        const auto it = rule_table.find(node_type);
        return it != rule_table.end() ? it->second : HighlightCategory::Plain;
    };
    // The incremental-parse tree cache is keyed per BLOCK (block_id), not
    // just per language: two same-language code blocks rendered in the same
    // document previously shared one "language" slot and thrashed each
    // other's incremental base every debounce tick (each Highlight() call for
    // block B evicted block A's cached tree, so both fell back to a full
    // reparse every tick). block_id==0 (no identity offered) keeps the
    // original language-only key so existing callers (tests, HighlightCode
    // direct callers) are unaffected.
    const std::string tree_key =
        block_id == 0 ? canonical : canonical + '#' + std::to_string(block_id);

    // Incremental parse: feed the previous tree for this language as the base
    // so a still-growing block's reparse only re-examines what changed (the
    // dominant cost on the streaming path, where every debounce tick used to
    // rebuild the whole tree from scratch). tree-sitter requires an edited
    // tree describing exactly how the source changed -- an unedited tree from
    // DIFFERENT source silently reuses old nodes against text that never
    // produced them (two unrelated blocks sharing a tree; a streamed token
    // like "ret" -> "return" keeping its old category). Offer the previous
    // tree back only when this call's code is verifiably that previous
    // source with a suffix appended -- the actual streaming-growth case this
    // optimizes -- and edit it first; anything else (a different block, a
    // shrink, a mid-string edit) gets a full reparse (nullptr base), same as
    // a cache miss.
    TSTree* old_tree = parser_cache_->LastTree(tree_key);
    const std::string* old_code = parser_cache_->LastSource(tree_key);
    TSPoint new_end_point{0, 0};
    bool edited_old_tree = false;
    if (old_tree != nullptr && old_code != nullptr && old_code->size() <= code.size() &&
        code.compare(0, old_code->size(), *old_code) == 0) {
        const TSPoint old_end_point = parser_cache_->LastEndPoint(tree_key);
        new_end_point = AdvancePoint(old_end_point, code.substr(old_code->size()));
        TSInputEdit edit{};
        edit.start_byte = static_cast<uint32_t>(old_code->size());
        edit.old_end_byte = static_cast<uint32_t>(old_code->size());
        edit.new_end_byte = static_cast<uint32_t>(code.size());
        edit.start_point = old_end_point;
        edit.old_end_point = old_end_point;
        edit.new_end_point = new_end_point;
        ts_tree_edit(old_tree, &edit);
        edited_old_tree = true;
    } else {
        old_tree = nullptr;   // not a valid base -- full reparse
        new_end_point = AdvancePoint(TSPoint{0, 0}, code);
    }
    TSTree* tree =
        ts_parser_parse_string(parser, old_tree, code.data(), static_cast<uint32_t>(code.size()));
    if (tree == nullptr) {
        // Rare (a parser with no cancellation/timeout set essentially never
        // returns nullptr for merely invalid syntax -- tree-sitter produces
        // ERROR nodes instead). If we just edited old_tree in place above,
        // the cached tree and last_code_/last_point_ (still the PRE-edit
        // values) now disagree about what the tree reflects -- drop the
        // cache entry rather than risk a future edit compounding on an
        // already-edited-but-never-reparsed tree. A tree we left untouched
        // (the full-reparse branch) is still a valid base to retry with, same
        // as today.
        if (edited_old_tree) parser_cache_->Invalidate(tree_key);
        return {{HighlightCategory::Plain, 0, static_cast<uint32_t>(code.size())}};
    }

    // Bound the walk to what tree-sitter itself certifies changed: for the
    // append-only incremental case (edited_old_tree), ts_tree_get_changed_ranges
    // between the pre-parse edited tree and the fresh one returns the byte
    // ranges where old and new trees actually differ -- everything strictly
    // before the earliest such range is PROVEN identical (same node types,
    // same byte extents, no shift, since the edit only ever inserts at/after
    // the old text's end), so the previous call's spans for that prefix are
    // still valid and the traversal below skips re-walking their subtrees
    // entirely. This is what turns per-tick collection cost from O(current
    // block size) into O(what actually changed) for a long streaming block --
    // without it, Collect() re-walked the whole tree from the root every 120ms
    // tick even though ts_parser_parse_string above was already incremental.
    uint32_t skip_before = 0;
    if (edited_old_tree && old_tree != nullptr) {
        uint32_t range_count = 0;
        TSRange* ranges = ts_tree_get_changed_ranges(old_tree, tree, &range_count);
        if (ranges != nullptr) {
            if (range_count > 0) {
                uint32_t min_start = ranges[0].start_byte;
                for (uint32_t i = 1; i < range_count; ++i) {
                    min_start = std::min(min_start, ranges[i].start_byte);
                }
                skip_before = min_start;
            }
            free(ranges);
        }
    }
    parser_cache_->AdoptTree(tree_key, tree, std::string(code), new_end_point);

    std::vector<HighlightSpan> spans;
    if (skip_before > 0) {
        if (const std::vector<HighlightSpan>* cached = parser_cache_->LastSpans(tree_key);
            cached != nullptr) {
            for (const HighlightSpan& s : *cached) {
                if (s.end_byte <= skip_before) spans.push_back(s);
            }
        }
    }

    // parent_type_stack.back() is the type of the CURRENT cursor node's
    // parent (empty at the root) -- tracked alongside the cursor's own
    // descend/sibling/climb steps below, at the cost of one push per descend
    // and one pop per goto_parent, so FunctionFieldRule matching (which needs
    // "what is my parent, and which of its fields am I") never needs a
    // separate ts_node_parent() walk per node.
    std::vector<const char*> parent_type_stack;
    TSTreeCursor cursor = ts_tree_cursor_new(ts_tree_root_node(tree));
    bool descend = true;
    while (true) {
        if (descend) {
            const TSNode node = ts_tree_cursor_current_node(&cursor);
            if (skip_before > 0 && ts_node_end_byte(node) <= skip_before) {
                // Entirely within the region ts_tree_get_changed_ranges says
                // is unchanged; its spans are already in `spans` from the
                // cache above. Don't descend into it, don't re-append it --
                // just treat it as a leaf for traversal purposes.
                descend = false;
            } else {
                HighlightCategory category = categorize(ts_node_type(node));
                if (category == HighlightCategory::Plain && vendored != nullptr &&
                    !vendored->function_fields.empty()) {
                    const char* field = ts_tree_cursor_current_field_name(&cursor);
                    const char* parent_type =
                        parent_type_stack.empty() ? nullptr : parent_type_stack.back();
                    if (field != nullptr && parent_type != nullptr) {
                        for (const FunctionFieldRule& rule : vendored->function_fields) {
                            if (rule.parent_type == parent_type && rule.field_name == field) {
                                category = HighlightCategory::Function;
                                break;
                            }
                        }
                    }
                }
                // include_plain=false (the render path) skips the informational
                // Plain spans entirely -- Highlight() discarded them anyway; the
                // public HighlightCode keeps them for the span-level tests.
                if ((ts_node_is_named(node) && include_plain) ||
                    category != HighlightCategory::Plain) {
                    AppendSpan(spans, category, ts_node_start_byte(node), ts_node_end_byte(node));
                }
            }
        }
        if (descend) {
            const TSNode current = ts_tree_cursor_current_node(&cursor);
            if (ts_tree_cursor_goto_first_child(&cursor)) {
                parent_type_stack.push_back(ts_node_type(current));
                continue;
            }
        }
        if (ts_tree_cursor_goto_next_sibling(&cursor)) {
            descend = true;
        } else {
            // Climb until a parent has an unvisited sibling, or the root.
            bool moved = false;
            while (ts_tree_cursor_goto_parent(&cursor)) {
                if (!parent_type_stack.empty()) parent_type_stack.pop_back();
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
    parser_cache_->AdoptSpans(tree_key, spans);
    // `tree` stays owned by the parser cache: it is the incremental base for
    // this block's next (likely grown) reparse, and a completed block never
    // re-enters here at all -- the whole-result (language, code) cache above
    // this call answers those.
    return spans;
}

std::vector<HighlightSpan> ApplySpanOverrides(std::string_view code,
                                              const std::vector<HighlightSpan>& spans) {
    const uint32_t total = static_cast<uint32_t>(code.size());
    // Per-byte category: -1 = unclaimed. Spans arrive in preorder (ancestors
    // before descendants), so painting in REVERSE lets each descendant claim
    // its bytes first and the ancestor fill only what is left -- deeper wins.
    // The first draft painted forward, which made the ANCESTOR swallow every
    // categorized child (a JS template literal rendered as one String block).
    std::vector<int> claimed(total, -1);
    for (auto it = spans.rbegin(); it != spans.rend(); ++it) {
        if (it->category == HighlightCategory::Plain) continue;
        const uint32_t begin = std::min(it->start_byte, total);
        const uint32_t end = std::min(it->end_byte, total);
        for (uint32_t i = begin; i < end; ++i) {
            if (claimed[i] == -1) claimed[i] = static_cast<int>(it->category);
        }
    }
    std::vector<HighlightSpan> runs;
    uint32_t i = 0;
    while (i < total) {
        const HighlightCategory category =
            claimed[i] >= 0 ? static_cast<HighlightCategory>(claimed[i])
                            : HighlightCategory::Plain;
        uint32_t j = i;
        while (j < total && claimed[j] == claimed[i]) ++j;
        runs.push_back({category, i, j});
        i = j;
    }
    return runs;
}

ftxui::Elements SyntaxHighlighter::Highlight(std::string_view code, std::string_view language,
                                             std::size_t block_id) const {
    // MRU-ordered linear scan: kHighlightCacheCap (64) is small enough that a
    // handful of string compares is cheaper than the hashing/bucketing this
    // would need to be an unordered_map, and simple enough that it needs no
    // custom hasher for a (language, code) pair. theme_ is FIXED FOR THE
    // OBJECT's lifetime (bound at construction) even though its CONTENTS may
    // be swapped in place (/theme); a swap calls OnThemeChanged(), which
    // clears this cache precisely because the entries bake the old colors in.
    auto prefix_match = highlight_cache_.end();
    for (auto it = highlight_cache_.begin(); it != highlight_cache_.end(); ++it) {
        if (it->language != language) continue;
        if (it->code == code) {
            if (it != highlight_cache_.begin()) {
                highlight_cache_.splice(highlight_cache_.begin(), highlight_cache_, it);
            }
            return highlight_cache_.front().lines;
        }
        // A streaming code block grows by append every debounce tick, so
        // this call's `code` never matches an earlier tick's entry exactly
        // -- without this, every tick would push a brand-new near-duplicate
        // prefix entry instead of recognizing "this is the same block, just
        // longer," retaining up to kHighlightCacheCap full copies of a
        // growing string (plus its materialized Elements tree) for one
        // block. Remember the first (most-recently-used) entry this code is
        // a strict extension of, so it can be dropped once the new one is
        // computed below.
        if (prefix_match == highlight_cache_.end() && code.size() > it->code.size() &&
            code.substr(0, it->code.size()) == it->code) {
            prefix_match = it;
        }
    }

    const std::vector<HighlightSpan> spans =
        ApplySpanOverrides(code, Collect(code, language, /*include_plain=*/false, block_id));

    ftxui::Elements lines;
    ftxui::Elements current_line;
    uint32_t pos = 0;

    // The seven category colors resolved ONCE per call: emit_text used to call
    // Theme::ColorFor per span, allocating a std::string key every time.
    const std::array<ftxui::Color, 8> category_colors = {
        theme_.ColorFor(ThemeRoleFor(HighlightCategory::Keyword)),
        theme_.ColorFor(ThemeRoleFor(HighlightCategory::String)),
        theme_.ColorFor(ThemeRoleFor(HighlightCategory::Comment)),
        theme_.ColorFor(ThemeRoleFor(HighlightCategory::Number)),
        theme_.ColorFor(ThemeRoleFor(HighlightCategory::Function)),
        theme_.ColorFor(ThemeRoleFor(HighlightCategory::Type)),
        theme_.ColorFor(ThemeRoleFor(HighlightCategory::Operator)),
        theme_.ColorFor(ThemeRoleFor(HighlightCategory::Plain)),
    };
    const auto color_of = [&](HighlightCategory category) {
        return category_colors[static_cast<std::size_t>(category)];
    };

    const auto flush_line = [&]() {
        lines.push_back(current_line.empty() ? ftxui::text("") : ftxui::hbox(current_line));
        current_line.clear();
    };
    const auto emit_text = [&](std::string_view text, HighlightCategory category) {
        ftxui::Element element = ftxui::text(std::string(text));
        const ftxui::Color c = color_of(category);
        // Plain included: syntax_plain used to be looked up and then skipped,
        // leaving untagged tokens in the terminal's own foreground.
        if (c != ftxui::Color::Default) {
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
        // ApplySpanOverrides already produced ordered, non-overlapping runs
        // covering the input: Plain RUNS here are real unclaimed text (not the
        // informational spans the traversal emitted), so they render as plain
        // text -- skipping them would drop bytes.
        emit_range(span.start_byte, span.end_byte, span.category);
        pos = std::max(pos, span.end_byte);
    }
    // Trailing text after the last span.
    emit_range(pos, static_cast<uint32_t>(code.size()), HighlightCategory::Plain);
    flush_line();

    if (prefix_match != highlight_cache_.end()) highlight_cache_.erase(prefix_match);
    highlight_cache_.push_front({std::string(language), std::string(code), lines});
    if (highlight_cache_.size() > kHighlightCacheCap) highlight_cache_.pop_back();
    return lines;
}

}  // namespace minimal::client
