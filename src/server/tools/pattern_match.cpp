#include "minimal/tools/pattern_match.hpp"

namespace minimal::tools {
namespace {

// One wildcard level of the match. Recursion depth is bounded by the number
// of wildcards in the pattern -- operator config, sanity-checked to a few
// hundred short rules -- so the naive backtracking shape costs nothing real
// and is far easier to audit than a table-driven matcher.
bool MatchFrom(std::string_view pattern, std::string_view text, bool slash_terminates) {
    while (!pattern.empty()) {
        char c = pattern.front();
        if (c == '*') {
            // Collapse a run of stars: one, two, or more all behave as the
            // same wildcard, so "***/" can never sneak past the slash rule.
            std::size_t stars = 0;
            while (stars < pattern.size() && pattern[stars] == '*') ++stars;
            const bool crosses = !slash_terminates || stars >= 2;
            pattern.remove_prefix(stars);
            // Try every split point; in the path flavor a run containing '/'
            // is unreachable, so stop extending at the first slash.
            for (std::size_t take = 0;; ++take) {
                if (MatchFrom(pattern, text.substr(take), slash_terminates)) return true;
                if (take >= text.size()) return false;
                if (!crosses && text[take] == '/') return false;
            }
        }
        if (c == '\\') {
            // Escape: the next character matches literally, whatever it is.
            // Trailing escape matches nothing (fail closed), not the
            // backslash itself.
            if (pattern.size() < 2) return false;
            pattern.remove_prefix(1);
            c = pattern.front();
        }
        if (text.empty()) return false;
        if (c == '?') {
            if (slash_terminates && text.front() == '/') return false;
        } else if (text.front() != c) {
            return false;
        }
        pattern.remove_prefix(1);
        text.remove_prefix(1);
    }
    return text.empty();
}

}  // namespace

bool GlobMatch(std::string_view pattern, std::string_view text, bool slash_terminates) {
    return MatchFrom(pattern, text, slash_terminates);
}

bool GlobShaped(std::string_view pattern) {
    return pattern.find('*') != std::string_view::npos ||
           pattern.find('?') != std::string_view::npos;
}

}  // namespace minimal::tools
