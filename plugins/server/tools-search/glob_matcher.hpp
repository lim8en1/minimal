#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace minimal::tools_search {

// Translate a glob pattern into an RE2 regex source string.
//
// Semantics (the VS Code / ripgrep flavor):
//   *      any run of characters EXCEPT '/'
//   ?      any single character except '/'
//   **     any run of characters including '/'; the three special placements
//          `**/`, `/**`, and `/**/` mean "zero or more directories"
//   [...]  character class, `!` after `[` negates (rewritten to `^`)
//   {a,b}  alternation -- ONLY with kBraceAlternation. .gitignore patterns
//          are NOT passed this flag: git treats `{` as a literal character,
//          and the two callers must not silently agree to differ.
//
// The result is an UNANCHORED body: anchoring is the caller's rule, not the
// translator's. The glob tool always anchors (`^...$`: `*.cpp` is top level
// only, `**/*.cpp` is everywhere); gitignore and grep's glob filter anchor a
// pattern containing '/' and let one without match a basename at any depth.
enum class GlobFlags : unsigned {
    kNone = 0,
    kBraceAlternation = 1u << 0,  // {a,b} -> (?:a|b)
};

constexpr GlobFlags operator|(GlobFlags a, GlobFlags b) {
    return static_cast<GlobFlags>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
}
constexpr bool HasFlag(GlobFlags value, GlobFlags flag) {
    return (static_cast<unsigned>(value) & static_cast<unsigned>(flag)) != 0;
}

// Returns nullopt when the pattern cannot be translated (unterminated `[`,
// unbalanced `{`) -- the caller decides whether that is an error (glob tool)
// or a rule that can never match (gitignore, which must not fail a whole file
// over one bad line).
std::optional<std::string> GlobToRegex(std::string_view glob, GlobFlags flags);

// True when the pattern contains no glob metacharacters, so it names at most
// one concrete path -- the glob tool's fast path.
bool IsLiteralGlob(std::string_view glob);

}  // namespace minimal::tools_search
