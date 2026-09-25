#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace minimal::tools_search {

// .gitignore semantics, the ripgrep-compatible subset:
//
//   - blank lines and `#` comments are skipped (`\#` / `\!` escape a literal)
//   - a leading `!` negates (re-includes); the LAST matching rule in the
//     deepest .gitignore wins, which is how re-inclusion works
//   - a trailing `/` makes the rule match directories only
//   - a pattern containing `/` (other than a trailing one) is anchored to the
//     directory containing the .gitignore; otherwise it matches a basename at
//     any depth
//   - `**` has the three special placements (`**/`, `/**`, `/**/`); other
//     consecutive asterisks collapse to `*`
//
// Deliberate deviation from git: the git INDEX is never consulted (no git
// dependency), so untracked-but-unignored files are searched -- exactly
// ripgrep's default behaviour. Braces are literal characters here, as in git.
//
// One thing git itself cannot do, and neither can this: re-include a file
// inside a directory the same or a shallower level ignored. The walker prunes
// ignored directories and never descends into them.

// One parsed .gitignore file.
class GitignoreSet {
public:
    GitignoreSet();
    ~GitignoreSet();
    GitignoreSet(GitignoreSet&&);
    GitignoreSet& operator=(GitignoreSet&&);

    // Parse failure is not a thing: a line whose pattern cannot be translated
    // (e.g. an unterminated `[`) yields a rule that can never match, so one
    // malformed line cannot take a whole file's rules out of play.
    static GitignoreSet Parse(std::string_view content);

    // `rel` is the candidate path relative to the directory containing this
    // .gitignore, POSIX separators. `is_dir` matters because dir-only rules
    // match only directories. Returns the verdict of the LAST matching rule
    // within this set (nullopt when nothing here matched).
    enum class Verdict { kIgnore, kKeep };
    std::optional<Verdict> Match(std::string_view rel, bool is_dir) const;

private:
    struct Rule;  // owns a compiled re2::RE2; held by pointer (RE2 is immovable)
    std::vector<std::unique_ptr<Rule>> rules_;
};

// The stack of .gitignore sets in effect along the path from the search root
// to the current directory. The walker pushes one set per directory that
// carries a .gitignore and pops on the way out; `Match` consults every set in
// order and the deepest set with a verdict wins, which is git's precedence.
class GitignoreStack {
public:
    struct Entry {
        // Depth = number of path components from the search root to the
        // directory carrying this set. A rel path's components past `depth`
        // are this set's slice.
        int depth = 0;
        GitignoreSet set;
        // For a set from an ANCESTOR of the search root (depth 0): the search
        // root's path below that ancestor, with a trailing '/' ("src/"). It
        // is prepended to every slice, so the set matches paths relative to
        // its own directory -- what anchored patterns mean. Empty otherwise.
        std::string base;
    };

    void Push(int depth, GitignoreSet set, std::string base = {});
    void Pop();
    // `rel`: path relative to the search root, POSIX separators. True when the
    // final verdict is "ignore".
    bool IsIgnored(std::string_view rel, bool is_dir) const;
    bool empty() const { return entries_.empty(); }

private:
    std::vector<Entry> entries_;
};

}  // namespace minimal::tools_search
