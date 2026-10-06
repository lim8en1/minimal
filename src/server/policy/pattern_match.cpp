#include "minimal/tools/pattern_match.hpp"

#include <unordered_map>
#include <utility>
#include <vector>

#include "minimal/tools/match_policy.hpp"

namespace minimal::tools {

bool GlobMatchReference(std::string_view pattern, std::string_view text, bool slash_terminates) {
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

CompiledGlob::CompiledGlob(std::string_view pattern) {
    for (std::size_t i = 0; i < pattern.size();) {
        const char c = pattern[i];
        if (c == '*') {
            std::size_t stars = 0;
            while (i + stars < pattern.size() && pattern[i + stars] == '*') ++stars;
            // A run of stars is one wildcard; two or more cross '/' in the path
            // flavor ("***/" can never sneak past the slash rule).
            tokens_.push_back({stars >= 2 ? Kind::kStarStar : Kind::kStar, '*'});
            i += stars;
        } else if (c == '?') {
            tokens_.push_back({Kind::kAnyOne, '?'});
            ++i;
        } else if (c == '\\') {
            if (i + 1 >= pattern.size()) {  // trailing escape: fail closed
                never_ = true;
                return;
            }
            tokens_.push_back({Kind::kLiteral, pattern[i + 1]});
            i += 2;
        } else {
            tokens_.push_back({Kind::kLiteral, c});
            ++i;
        }
    }

    all_literal_ = true;
    for (const Token& token : tokens_) all_literal_ = all_literal_ && token.kind == Kind::kLiteral;
    if (all_literal_) {
        for (const Token& token : tokens_) literal_.push_back(token.c);
        return;
    }
    // Necessary conditions for a match, from the fixed parts of the pattern.
    std::size_t first = 0;
    while (first < tokens_.size() && tokens_[first].kind == Kind::kLiteral) {
        prefix_.push_back(tokens_[first++].c);
    }
    std::size_t last = tokens_.size();
    while (last > first && tokens_[last - 1].kind == Kind::kLiteral) --last;
    for (std::size_t i = last; i < tokens_.size(); ++i) suffix_.push_back(tokens_[i].c);
    std::string run;
    for (const Token& token : tokens_) {
        if (token.kind == Kind::kLiteral) {
            run.push_back(token.c);
            if (run.size() > required_.size()) required_ = run;
        } else {
            run.clear();
        }
    }
}

bool CompiledGlob::Match(std::string_view text, bool slash_terminates) const {
    if (never_) return false;
    if (all_literal_) return text == literal_;
    if (text.size() < prefix_.size() + suffix_.size()) return false;
    if (text.substr(0, prefix_.size()) != prefix_) return false;
    if (text.substr(text.size() - suffix_.size()) != suffix_) return false;
    if (!required_.empty() && text.find(required_) == std::string_view::npos) return false;

    // Rolling form of the reference's table: row `next` is "does tokens[i+1:]
    // match text[ti:]", for every ti. Scratch rows are reused per thread, so a
    // survivor of the filters above costs no allocation either.
    const std::size_t n = text.size();
    thread_local std::vector<unsigned char> row_a;
    thread_local std::vector<unsigned char> row_b;
    row_a.assign(n + 1, 0);
    row_b.assign(n + 1, 0);
    unsigned char* next = row_a.data();
    unsigned char* cur = row_b.data();
    next[n] = 1;  // no tokens left matches only the empty remainder

    for (std::size_t pi = tokens_.size(); pi-- > 0;) {
        const Token& token = tokens_[pi];
        if (token.kind == Kind::kStar || token.kind == Kind::kStarStar) {
            const bool crosses = !slash_terminates || token.kind == Kind::kStarStar;
            bool running = next[n] != 0;
            cur[n] = running;
            for (std::size_t ti = n; ti-- > 0;) {
                running = (!crosses && text[ti] == '/') ? next[ti] != 0 : (next[ti] != 0 || running);
                cur[ti] = running;
            }
        } else {
            for (std::size_t ti = 0; ti < n; ++ti) {
                const bool char_ok = token.kind == Kind::kAnyOne
                                         ? !(slash_terminates && text[ti] == '/')
                                         : text[ti] == token.c;
                cur[ti] = char_ok && next[ti + 1];
            }
            cur[n] = 0;  // no character left to consume
        }
        std::swap(cur, next);
    }
    return next[0] != 0;
}

bool GlobMatch(std::string_view pattern, std::string_view text, bool slash_terminates) {
    // Per-thread cache of compiled patterns, keyed by the pattern text: no lock on
    // the hot path, and a changed rule is simply a different key. Bounded -- a
    // process that matches an unbounded stream of distinct patterns starts over
    // rather than growing.
    thread_local std::unordered_map<std::string, CompiledGlob> cache;
    auto it = cache.find(std::string(pattern));
    if (it == cache.end()) {
        if (cache.size() >= kMaxCachedGlobs) cache.clear();
        it = cache.emplace(std::string(pattern), CompiledGlob(pattern)).first;
    }
    return it->second.Match(text, slash_terminates);
}

bool GlobShaped(std::string_view pattern) {
    return pattern.find('*') != std::string_view::npos ||
           pattern.find('?') != std::string_view::npos;
}

}  // namespace minimal::tools
