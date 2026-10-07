#include "syntax_grammars.hpp"

#include <array>
#include <utility>
#include <vector>

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

namespace minimal::client::syntax_grammars {

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
    std::vector<FunctionFieldRule> function_fields;
    std::vector<std::string_view> aliases;  // fence labels that mean this language
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

const auto& VendoredTable() {
    static const auto kLanguages = [] {
        auto languages = std::to_array<Language>({
            CppLanguage(),   PythonLanguage(), JavascriptLanguage(), TypescriptLanguage(),
            BashLanguage(),  JsonLanguage(),   YamlLanguage(),       RustLanguage(),
            GoLanguage(),
        });
        // The fence labels that mean each language. (Set here, not in each
        // XLanguage(): TypescriptLanguage starts from javascript's.)
        const std::pair<std::string_view, std::vector<std::string_view>> kAliases[] = {
            {"cpp", {"c++", "cc", "cxx", "h", "hpp", "c", "hh", "hxx"}},
            {"python", {"py", "python3"}},
            {"javascript", {"js", "jsx", "node", "mjs", "cjs"}},
            {"typescript", {"ts", "tsx"}},
            {"bash", {"sh", "shell", "zsh"}},
            {"json", {"jsonc", "json5"}},
            {"yaml", {"yml"}},
            {"rust", {"rs"}},
            {"go", {"golang"}},
        };
        for (auto& language : languages) {
            for (const auto& [name, aliases] : kAliases) {
                if (language.name == name) language.aliases = aliases;
            }
        }
        return languages;
    }();
    return kLanguages;
}

// One vendored language as the grammar slot takes it.
class VendoredGrammar final : public plugins::SyntaxGrammar {
public:
    explicit VendoredGrammar(const struct Language& language) : language_(language) {}
    std::string_view Language() const override { return language_.name; }
    LanguageFnPtr LanguageFn() const override { return language_.language_fn; }
    std::span<const SyntaxRule> Rules() const override { return language_.rules; }
    std::span<const FunctionFieldRule> FunctionFields() const override {
        return language_.function_fields;
    }
    std::span<const std::string_view> Aliases() const override { return language_.aliases; }

private:
    const struct Language& language_;  // VendoredTable()'s: static
};

}  // namespace

void RegisterGrammars(plugins::SyntaxGrammarRegistry& registry) {
    for (const Language& language : VendoredTable()) {
        registry.Register(std::make_unique<VendoredGrammar>(language));
    }
}

std::vector<LanguageDefinition> Definitions() {
    std::vector<LanguageDefinition> out;
    for (const Language& language : VendoredTable()) {
        out.push_back({language.name, language.language_fn, language.rules,
                       language.function_fields, language.aliases});
    }
    return out;
}

}  // namespace minimal::client::syntax_grammars
