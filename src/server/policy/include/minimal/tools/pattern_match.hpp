#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace minimal::tools {

// Glob matching for permission pattern rules. Two flavors, because the slash
// means different things to the two subjects:
//
//   - PATHS (slash_terminates = true): matched against a canonical absolute
//     path. '*' and '?' stay within one path component; '**' crosses them.
//     "**/.env" matches "/work/proj/.env" and "/work/proj/sub/.env" alike.
//
//   - BASH COMMANDS (slash_terminates = false): a command line is not a path,
//     so '/' is an ordinary character and '*' crosses it. "git *" matches
//     "git status src/file.cpp".
//
// A pattern matches the WHOLE subject, never a substring. '*' and '?' never
// match a '/' in the path flavor (write "**" for "anywhere"); there is no
// character-class or brace syntax -- operator config, kept simple on purpose.
// A malformed pattern (unbalanced escape) matches nothing rather than
// everything -- the fail-closed direction.
//
// Compiled forms are cached per thread, so a pattern a policy applies to many
// subjects (a file walk asks once per file) is parsed once, and most
// non-matching subjects are rejected by cheap necessary conditions (fixed prefix,
// fixed suffix, required literal) before any table is built.
bool GlobMatch(std::string_view pattern, std::string_view text, bool slash_terminates);

// The specification: the straightforward bottom-up matcher GlobMatch's result
// must always equal. Kept so tests can check the optimized path against it over
// generated patterns; not for production callers.
bool GlobMatchReference(std::string_view pattern, std::string_view text, bool slash_terminates);

// A pattern parsed once. Same semantics as GlobMatch (which caches these).
class CompiledGlob {
public:
    explicit CompiledGlob(std::string_view pattern);
    bool Match(std::string_view text, bool slash_terminates) const;

private:
    enum class Kind : unsigned char { kLiteral, kAnyOne, kStar, kStarStar };
    struct Token {
        Kind kind;
        char c;
    };
    std::vector<Token> tokens_;
    bool never_ = false;        // malformed (trailing escape): matches nothing
    bool all_literal_ = false;  // no wildcard: plain equality with `literal_`
    std::string literal_;       // the unescaped text, when all_literal_
    std::string prefix_;        // fixed text every match starts with
    std::string suffix_;        // fixed text every match ends with
    std::string required_;      // longest literal run; must occur in the text
};

// A pattern rule "carries a wildcard" when it cannot match as a bare literal.
// The frontmatter alias layer uses this to route map values: a scalar
// ("bash": "allow") stays an exact per-tool override, a wildcard-shaped value
// ("git *": "allow") becomes a pattern rule.
bool GlobShaped(std::string_view pattern);

}  // namespace minimal::tools
