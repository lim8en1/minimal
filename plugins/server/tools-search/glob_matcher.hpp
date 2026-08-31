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
//   {a,b}  alternation -- ONLY when `brace_alternation` is set. .gitignore
//          patterns are NOT passed this flag: git treats `{` as a literal
//          character, and the two callers must not silently agree to differ.
//
// `slash_anchors`: when true, a pattern containing a '/' is anchored to the
// root of the searched tree; a pattern without one may match a basename at
// any depth. This is the gitignore rule (see ignore_rules.hpp). The glob tool
// and the grep glob-filter instead pass `force_anchored` -- `*.cpp` means
// "top level only", `**/*.cpp` means "everywhere".
enum class GlobFlags : unsigned {
    kNone = 0,
    kBraceAlternation = 1u << 0,  // {a,b} -> (?:a|b)
    kForceAnchored = 1u << 1,     // always anchor, ignore the slash heuristic
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

// Convenience: translate, and report failure through the optional. Returns the
// regex SOURCE -- RE2 is neither copyable nor movable, so handing back a
// compiled object by value is not expressible; callers construct their own.
std::optional<std::string> CompileGlob(std::string_view glob, GlobFlags flags);

}  // namespace minimal::tools_search
