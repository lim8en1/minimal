#include "glob_matcher.hpp"

namespace minimal::tools_search {
namespace {

void EmitEscaped(std::string& out, char c) {
    // RE2 metacharacters that must be taken literally when they appear outside
    // the glob constructs handled by the scanner. `/` is never escaped -- rel
    // paths are POSIX throughout.
    switch (c) {
        case '.': case '+': case '(': case ')': case '^': case '$':
        case '|': case '\\': case '{': case '}':
        // `*?[]` normally never reach here -- TranslateBody consumes them as
        // glob constructs -- but the backslash-escape path routes a literal
        // one through, and a bare `*` in a regex is not a literal `*`.
        case '*': case '?': case '[': case ']':
            out += '\\';
            break;
        default:
            break;  // `*?[]` never reach here; anything else is inert in RE2
    }
    out += c;
}

// Translates one brace-free stretch of pattern. `out` accumulates. Returns
// false on an unterminated `[`.
bool TranslateBody(std::string_view glob, std::string& out) {
    const std::size_t n = glob.size();
    std::size_t i = 0;
    while (i < n) {
        const char c = glob[i];
        if (c == '*') {
            if (i + 1 < n && glob[i + 1] == '*') {
                // `**/` at the very start: zero or more leading directories.
                if (i == 0 && i + 2 < n && glob[i + 2] == '/') {
                    out += "(?:.*/)?";
                    i += 3;
                    continue;
                }
                // `/**` at the very end: everything beneath.
                if (i + 2 == n && i > 0 && glob[i - 1] == '/') {
                    out += ".*";
                    i += 2;
                    continue;
                }
                // `/**/` in the middle: the preceding `/` was already emitted.
                if (i + 2 < n && glob[i + 2] == '/' && i > 0 && glob[i - 1] == '/') {
                    out += "(?:.*/)?";
                    i += 3;
                    continue;
                }
                // Any other `**` collapses to a single `*` -- the git rule for
                // consecutive asterisks outside the three special placements.
                out += "[^/]*";
                i += 2;
                continue;
            }
            out += "[^/]*";
            i += 1;
            continue;
        }
        if (c == '?') {
            out += "[^/]";
            i += 1;
            continue;
        }
        if (c == '[') {
            // Find the terminator. A `]` immediately after `[` or `[!` is a
            // literal member (POSIX glob / regex rule), so scan from index 2.
            std::size_t close = std::string_view::npos;
            for (std::size_t j = i + 2; j < n; ++j) {
                if (glob[j] == ']') {
                    close = j;
                    break;
                }
            }
            if (close == std::string_view::npos) return false;
            out += '[';
            std::size_t body = i + 1;
            if (glob[body] == '!') {  // negate: `!` is glob, `^` is RE2
                out += '^';
                body += 1;
            }
            out.append(glob.substr(body, close - body));
            out += ']';
            i = close + 1;
            continue;
        }
        EmitEscaped(out, c);
        i += 1;
    }
    return true;
}

// Depth-aware scan for the `}` matching the `{` at `open`. npos = unbalanced.
std::size_t MatchingBrace(std::string_view glob, std::size_t open) {
    int depth = 0;
    for (std::size_t i = open; i < glob.size(); ++i) {
        if (glob[i] == '\\') {
            i += 1;  // escaped char, skip -- keeps `\{` from opening a group
            continue;
        }
        if (glob[i] == '{') depth += 1;
        if (glob[i] == '}') {
            depth -= 1;
            if (depth == 0) return i;
        }
    }
    return std::string_view::npos;
}

}  // namespace

std::optional<std::string> GlobToRegex(std::string_view glob, GlobFlags flags) {
    std::string out;
    const std::size_t n = glob.size();
    std::size_t i = 0;
    while (i < n) {
        if (glob[i] == '{' && HasFlag(flags, GlobFlags::kBraceAlternation)) {
            const std::size_t close = MatchingBrace(glob, i);
            if (close == std::string_view::npos) return std::nullopt;
            // Split top-level commas of the brace body, translating each
            // alternative recursively (nested braces are allowed).
            const std::string_view body = glob.substr(i + 1, close - i - 1);
            out += "(?:";
            int depth = 0;
            std::size_t start = 0;
            bool first_alternative = true;
            for (std::size_t j = 0; j <= body.size(); ++j) {
                const bool at_end = j == body.size();
                if (!at_end) {
                    if (body[j] == '\\') {
                        j += 1;  // keep escapes inside one alternative
                        continue;
                    }
                    if (body[j] == '{') depth += 1;
                    if (body[j] == '}') depth -= 1;
                    if (body[j] != ',' || depth > 0) continue;
                }
                const auto alternative = GlobToRegex(body.substr(start, j - start), flags);
                if (!alternative) return std::nullopt;
                if (!first_alternative) out += '|';
                first_alternative = false;
                out += *alternative;
                start = j + 1;
            }
            out += ')';
            i = close + 1;
            continue;
        }
        if (glob[i] == '}' && HasFlag(flags, GlobFlags::kBraceAlternation)) {
            return std::nullopt;  // an unmatched `}` inside brace mode is a
                                  // typo, not a literal -- fail the pattern
        }
        if (glob[i] == '\\') {
            // Backslash escape: take the next character literally.
            if (i + 1 >= n) return std::nullopt;
            EmitEscaped(out, glob[i + 1]);
            i += 2;
            continue;
        }
        const std::size_t seg_start = i;
        while (i < n && glob[i] != '{' && glob[i] != '}' && glob[i] != '\\') i += 1;
        if (i == seg_start) {
            // A brace metacharacter in literal mode (gitignore): the segment
            // scanner stopped without advancing, so consume it here or the
            // outer loop spins forever.
            EmitEscaped(out, glob[i]);
            i += 1;
            continue;
        }
        if (!TranslateBody(glob.substr(seg_start, i - seg_start), out)) return std::nullopt;
    }
    return out;
}

bool IsLiteralGlob(std::string_view glob) {
    return glob.find_first_of("*?[]{\\") == std::string_view::npos;
}

std::optional<std::string> CompileGlob(std::string_view glob, GlobFlags flags) {
    return GlobToRegex(glob, flags);
}

}  // namespace minimal::tools_search
