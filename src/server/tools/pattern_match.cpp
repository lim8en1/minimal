#include "minimal/tools/pattern_match.hpp"

#include <vector>

namespace minimal::tools {

bool GlobMatch(std::string_view pattern, std::string_view text, bool slash_terminates) {
    // A pattern with no metacharacter is plain equality: skip the table. Most
    // permission rules are literals, and this runs per rule per walked file.
    if (pattern.find_first_of("*?\\") == std::string_view::npos) return pattern == text;

    const std::size_t plen = pattern.size();
    const std::size_t tlen = text.size();

    // Bottom-up DP over (pattern-offset, text-offset), rather than the naive
    // top-down recursion this replaced: a pattern with several '*'s made that
    // recursion re-explore the SAME (offset, offset) pair once per ancestor
    // split point -- each star's "try every split point" loop recursed fresh
    // per split -- which is exponential in the number of stars against a
    // long, near-miss text (an ordinary multi-wildcard permission rule
    // against a long bash command line, say; confirmed empirically at
    // several seconds for 10 stars against 40 non-matching characters).
    //
    // ok[pi][ti] = does pattern[pi:] match text[ti:]? Computed for
    // decreasing pi so every transition target is already known. The '*'
    // case's "OR over every valid split point" is itself collapsed into one
    // O(text_length) suffix scan per pattern offset (not one scan per text
    // offset), so the whole table costs O(pattern_length * text_length) --
    // the classic bound for this problem, and fully iterative (no recursion
    // depth to bound at all, an improvement over the original's "bounded by
    // the number of wildcards").
    std::vector<std::vector<bool>> ok(plen + 1, std::vector<bool>(tlen + 1, false));
    ok[plen][tlen] = true;  // empty pattern matches only empty text

    for (std::size_t pi = plen; pi-- > 0;) {
        const char c = pattern[pi];
        if (c == '*') {
            // Collapse a run of stars: one, two, or more all behave as the
            // same wildcard, so "***/" can never sneak past the slash rule.
            std::size_t stars = 0;
            while (pi + stars < plen && pattern[pi + stars] == '*') ++stars;
            const bool crosses = !slash_terminates || stars >= 2;
            const std::size_t next_pi = pi + stars;

            // window_or[ti], built right to left: the OR of ok[next_pi][k]
            // for every k the star could still reach from ti -- k = ti
            // itself (zero characters taken) is always reachable; extending
            // to include window_or[ti+1]'s reach additionally requires that
            // consuming text[ti] (the newly-taken character) doesn't cross a
            // '/' the star isn't allowed to cross. In the path flavor a run
            // containing '/' is unreachable, so the running OR resets at
            // each slash instead of accumulating across it.
            bool running = ok[next_pi][tlen];
            ok[pi][tlen] = running;
            for (std::size_t ti = tlen; ti-- > 0;) {
                if (!crosses && text[ti] == '/') {
                    running = ok[next_pi][ti];
                } else {
                    running = ok[next_pi][ti] || running;
                }
                ok[pi][ti] = running;
            }
            continue;
        }

        std::size_t consumed = 1;
        char match_char = c;
        bool escaped = false;
        if (c == '\\') {
            // Escape: the next character matches literally, whatever it is --
            // including '?', which must NOT fall into the wildcard branch
            // below. Trailing escape matches nothing (fail closed), not the
            // backslash itself -- ok[pi][*] stays false for every ti, its
            // initialized value, so there is nothing further to compute.
            if (plen - pi < 2) continue;
            match_char = pattern[pi + 1];
            consumed = 2;
            escaped = true;
        }
        const std::size_t next_pi = pi + consumed;
        for (std::size_t ti = 0; ti < tlen; ++ti) {
            const bool char_ok = (!escaped && match_char == '?')
                                     ? !(slash_terminates && text[ti] == '/')
                                     : text[ti] == match_char;
            ok[pi][ti] = char_ok && ok[next_pi][ti + 1];
        }
        // ok[pi][tlen] stays false: no character left in `text` to consume.
    }

    return ok[0][0];
}

bool GlobShaped(std::string_view pattern) {
    return pattern.find('*') != std::string_view::npos ||
           pattern.find('?') != std::string_view::npos;
}

}  // namespace minimal::tools
