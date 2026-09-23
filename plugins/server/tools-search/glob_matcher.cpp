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
// Returns the index of the `]` that terminates the class starting at
// glob[i] == '[', honoring POSIX first-position literals and backslash
// escapes; npos when unterminated. Shared by TranslateBody and the outer
// segment scanner -- the scanner must not split a segment at a backslash
// that lives INSIDE a class.
std::size_t FindClassClose(std::string_view glob, std::size_t i) {
    const std::size_t n = glob.size();
    std::size_t j = i + 1;
    if (j < n && glob[j] == ']') {
        j += 1;  // `[]...` -- literal first
    } else if (j + 1 < n && glob[j] == '!' && glob[j + 1] == ']') {
        j += 2;  // `[!]...` -- literal first after negation
    }
    for (; j < n; ++j) {
        if (glob[j] == '\\' && j + 1 < n) {
            ++j;
            continue;
        }
        if (glob[j] == ']') return j;
    }
    return std::string_view::npos;
}

bool TranslateBody(std::string_view glob, std::string& out) {
    const std::size_t n = glob.size();
    std::size_t i = 0;
    while (i < n) {
        const char c = glob[i];
        if (c == '*') {
            if (i + 1 < n && glob[i + 1] == '*') {
                // A bare `**` (the WHOLE pattern, no leading/trailing `/` to
                // anchor it into one of the three special placements below)
                // hit none of them and fell to the "collapses to a single
                // `*`" rule, so it matched only a single path segment
                // instead of the whole tree the way ripgrep and VS Code
                // treat it. Every real `**` placement pairs with a `/`; the
                // whole-pattern case is the one with none at all to pair.
                if (i == 0 && i + 2 == n) {
                    out += ".*";
                    i += 2;
                    continue;
                }
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
            // POSIX rules: a `]` in FIRST position (or right after a `!`
            // negation) is a literal member, not the terminator. A backslash
            // escapes the next character anywhere in the class: `\]` is a
            // literal member, not the terminator (the body is copied
            // verbatim, so RE2 still sees the escape).
            const std::size_t close = FindClassClose(glob, i);
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

namespace {

// The translator's caps. RE2's linear-time guarantee -- the reason the
// dependency exists -- is moot if the TRANSLATION blows up first: nested brace
// alternation duplicated each level into the output (`{{a,b},{a,b}}` doubles),
// so a ~320-byte model-authored pattern reached a 2^40-byte string before RE2
// ever saw it, and the recursion depth was stack-bound. Depth 16 and a 64 KiB
// translation are far above every legitimate glob; past either the pattern is
// refused (nullopt -> the tool's error result), not matched.
constexpr int kMaxBraceDepth = 16;
constexpr std::size_t kMaxTranslatedBytes = 64ull << 10;

bool GlobToRegexImpl(std::string_view glob, GlobFlags flags, int depth, std::string& out) {
    if (depth > kMaxBraceDepth) return false;
    const std::size_t n = glob.size();
    std::size_t i = 0;
    while (i < n) {
        if (glob[i] == '{' && HasFlag(flags, GlobFlags::kBraceAlternation)) {
            const std::size_t close = MatchingBrace(glob, i);
            if (close == std::string_view::npos) return false;
            // Split top-level commas of the brace body, translating each
            // alternative recursively (nested braces are allowed) INTO `out`
            // -- the old shape translated each alternative to its OWN string
            // and appended, which is where the doubling hid.
            const std::string_view body = glob.substr(i + 1, close - i - 1);
            out += "(?:";
            int brace_depth = 0;
            std::size_t start = 0;
            bool first_alternative = true;
            for (std::size_t j = 0; j <= body.size(); ++j) {
                const bool at_end = j == body.size();
                if (!at_end) {
                    if (body[j] == '\\') {
                        j += 1;  // keep escapes inside one alternative
                        continue;
                    }
                    if (body[j] == '{') brace_depth += 1;
                    if (body[j] == '}') brace_depth -= 1;
                    if (body[j] != ',' || brace_depth > 0) continue;
                }
                if (!first_alternative) out += '|';
                first_alternative = false;
                if (!GlobToRegexImpl(body.substr(start, j - start), flags, depth + 1, out)) {
                    return false;
                }
                start = j + 1;
            }
            out += ')';
            i = close + 1;
            continue;
        }
        if (glob[i] == '}' && HasFlag(flags, GlobFlags::kBraceAlternation)) {
            return false;  // an unmatched `}` inside brace mode is a
                           // typo, not a literal -- fail the pattern
        }
        if (glob[i] == '\\') {
            // Backslash escape: take the next character literally.
            if (i + 1 >= n) return false;
            EmitEscaped(out, glob[i + 1]);
            i += 2;
            continue;
        }
        const std::size_t seg_start = i;
        while (i < n && glob[i] != '{' && glob[i] != '}' && glob[i] != '\\') {
            if (glob[i] == '[') {
                // A character class is consumed WHOLE, escapes included: the
                // old scanner broke at any backslash, so a class containing
                // `\]` was split and the pattern failed outright. An
                // unterminated class stays a failed pattern (the gitignore
                // rule "unterminated constructs fail"), never a literal.
                const std::size_t close = FindClassClose(glob, i);
                if (close == std::string_view::npos) return false;
                i = close + 1;
                continue;
            }
            i += 1;
        }
        if (i == seg_start) {
            // A brace metacharacter in literal mode (gitignore): the segment
            // scanner stopped without advancing, so consume it here or the
            // outer loop spins forever.
            EmitEscaped(out, glob[i]);
            i += 1;
            continue;
        }
        if (!TranslateBody(glob.substr(seg_start, i - seg_start), out)) return false;
        if (out.size() > kMaxTranslatedBytes) return false;
    }
    return out.size() <= kMaxTranslatedBytes;
}

}  // namespace

std::optional<std::string> GlobToRegex(std::string_view glob, GlobFlags flags) {
    std::string out;
    out.reserve(glob.size() * 2);
    if (!GlobToRegexImpl(glob, flags, 0, out)) return std::nullopt;
    return out;
}

bool IsLiteralGlob(std::string_view glob) {
    return glob.find_first_of("*?[]{\\") == std::string_view::npos;
}

std::optional<std::string> CompileGlob(std::string_view glob, GlobFlags flags) {
    return GlobToRegex(glob, flags);
}

}  // namespace minimal::tools_search
