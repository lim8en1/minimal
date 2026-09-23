#include "ignore_rules.hpp"

#include <memory>

#include <re2/re2.h>

#include "glob_matcher.hpp"

namespace minimal::tools_search {
namespace {

}  // namespace

struct GitignoreSet::Rule {
    bool negated = false;
    bool dir_only = false;
    // Whether the pattern is anchored to the set's directory (contains a `/`
    // after the trailing-slash strip, or starts with one). Anchored rules
    // match the whole rel slice; unanchored ones match a basename at any
    // depth -- which, for a walker that prunes ignored directories, is
    // exactly "match the final component".
    bool anchored = false;
    std::unique_ptr<re2::RE2> re;  // null when the pattern is untranslatable
};

GitignoreSet::GitignoreSet() = default;
GitignoreSet::~GitignoreSet() = default;
GitignoreSet::GitignoreSet(GitignoreSet&&) = default;
GitignoreSet& GitignoreSet::operator=(GitignoreSet&&) = default;

GitignoreSet GitignoreSet::Parse(std::string_view content) {
    GitignoreSet set;
    std::size_t pos = 0;
    while (pos < content.size()) {
        std::size_t end = content.find('\n', pos);
        const bool last_line = end == std::string_view::npos;
        if (last_line) end = content.size();
        std::string_view line = content.substr(pos, end - pos);
        pos = end + 1;

        // git strips trailing spaces unless backslash-escaped; the escaped
        // case is recovered by GlobToRegex itself (it unescapes `\c` to a
        // literal `c` as part of translating the pattern, the same place
        // every other escape in the line is resolved), so we only trim
        // unescaped runs here.
        while (!line.empty() && (line.back() == ' ' || line.back() == '\r') &&
               (line.size() < 2 || line[line.size() - 2] != '\\')) {
            line.remove_suffix(1);
        }
        if (line.empty() || line[0] == '#') continue;

        bool negated = false;
        if (line[0] == '!') {
            negated = true;
            line.remove_prefix(1);
        }
        if (line.empty()) continue;
        // An escaped leading `\!` or `\#` is a literal character, not a
        // negation or a comment marker: drop only the backslash.
        if (line[0] == '\\' && line.size() > 1 && (line[1] == '!' || line[1] == '#')) {
            line.remove_prefix(1);
        }

        bool dir_only = false;
        if (!line.empty() && line.back() == '/') {
            dir_only = true;
            line.remove_suffix(1);
        }
        if (line.empty()) continue;

        // Anchoring is decided BEFORE unescaping and AFTER the trailing-slash
        // strip: a trailing `/` does not make a pattern anchored, a leading
        // or interior one does.
        const bool anchored =
            line[0] == '/' || line.find('/') != std::string_view::npos;
        if (line[0] == '/') line.remove_prefix(1);
        if (line.empty()) continue;

        auto rule = std::make_unique<Rule>();
        rule->negated = negated;
        rule->dir_only = dir_only;
        rule->anchored = anchored;
        // Passed directly, not pre-unescaped: GlobToRegex already turns a
        // backslash-escaped character into its literal self as part of
        // translating the pattern. Unescaping first double-processed every
        // escape -- `\*.log` (a literal file named "*.log") became `*.log`
        // and ignored every .log file; `foo\[1\]` became a character class.
        const auto body = GlobToRegex(line, GlobFlags::kNone);
        if (body) {
            // FullMatch pins both ends; the `^`/`$` on anchored rules are for
            // readability. Unanchored rules get the any-depth prefix and are
            // matched against the candidate's final component.
            rule->re = std::make_unique<re2::RE2>(
                anchored ? "^" + *body + "$" : "(?:.*/)?" + *body + "$");
        }
        // !rule->re: one untranslatable line (e.g. an unterminated `[`)
        // becomes a rule that can never match -- a malformed line cannot take
        // the whole file's rules out of play.
        set.rules_.push_back(std::move(rule));

        if (last_line) break;
    }
    return set;
}

std::optional<GitignoreSet::Verdict> GitignoreSet::Match(std::string_view rel,
                                                          bool is_dir) const {
    // The LAST matching rule in this set wins -- that is git's negation
    // semantics ("last match decides"), and it is what makes re-inclusion
    // (`!keep.log` after `*.log`) work.
    std::optional<Verdict> verdict;
    for (const auto& rule : rules_) {
        if (!rule->re) continue;
        if (rule->dir_only && !is_dir) continue;
        std::string_view subject = rel;
        if (!rule->anchored) {
            const auto slash = rel.find_last_of('/');
            subject = slash == std::string_view::npos ? rel : rel.substr(slash + 1);
        }
        if (re2::RE2::FullMatch(subject, *rule->re)) {
            verdict = rule->negated ? Verdict::kKeep : Verdict::kIgnore;
        }
    }
    return verdict;
}

void GitignoreStack::Push(int depth, GitignoreSet set) {
    entries_.push_back({depth, std::move(set)});
}

void GitignoreStack::Pop() { entries_.pop_back(); }

bool GitignoreStack::IsIgnored(std::string_view rel, bool is_dir) const {
    // Depends only on `rel`, not on the entry being checked -- hoisted out of
    // the loop below so a path is scanned once per call, not once per
    // gitignore level on the stack.
    int components = 0;
    for (const char c : rel) {
        if (c == '/') components += 1;
    }

    std::optional<GitignoreSet::Verdict> verdict;
    for (const auto& entry : entries_) {
        // Each set sees only the slice of the path below its own directory;
        // a path shallower than the set's depth cannot match it.
        if (components < entry.depth) continue;
        std::string_view slice = rel;
        for (int skips = 0; skips < entry.depth && !slice.empty(); ++skips) {
            slice = slice.substr(slice.find('/') + 1);
        }
        if (slice.empty()) continue;
        const auto set_verdict = entry.set.Match(slice, is_dir);
        if (set_verdict) verdict = set_verdict;  // deeper sets override shallower
    }
    return verdict == GitignoreSet::Verdict::kIgnore;
}

}  // namespace minimal::tools_search
