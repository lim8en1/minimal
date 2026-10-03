#pragma once

#include <string_view>

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
bool GlobMatch(std::string_view pattern, std::string_view text, bool slash_terminates);

// A pattern rule "carries a wildcard" when it cannot match as a bare literal.
// The frontmatter alias layer uses this to route map values: a scalar
// ("bash": "allow") stays an exact per-tool override, a wildcard-shaped value
// ("git *": "allow") becomes a pattern rule.
bool GlobShaped(std::string_view pattern);

}  // namespace minimal::tools
