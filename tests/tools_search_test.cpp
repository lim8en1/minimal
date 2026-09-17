// tools-search: glob matcher, gitignore rules, tree walker, and the two
// tools. Pure logic (tier 1) plus real-filesystem traversal against a temp
// project_root (tier 3) -- traversal is exactly the layer a fake would paper
// over, so nothing here fakes the filesystem.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <fstream>
#include <optional>
#include <thread>

#include <re2/re2.h>

#include "glob_matcher.hpp"
#include "grep_tool.hpp"
#include "ignore_rules.hpp"
#include "search_settings.hpp"
#include "temp_dir.hpp"
#include "glob_tool.hpp"
#include "tree_walker.hpp"

using namespace minimal;
using minimal::testing::TempDir;
using tools_search::GlobFlags;

namespace {

bool Matches(const std::string& regex_source, const std::string& subject) {
    const re2::RE2 re(regex_source);
    return re.ok() && re2::RE2::FullMatch(subject, re);
}

struct Fixture {
    TempDir dir;
    tools_search::SearchSettings settings;

    tools::ToolContext Context() {
        tools::ToolContext ctx{common::DefaultLogger()};
        ctx.session_id = "s1";
        ctx.project_root = dir.path();
        return ctx;
    }

    std::filesystem::path Write(const std::string& name, const std::string& content) {
        const auto path = dir.path() / name;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary);
        out << content;
        return path;
    }

    void WriteIgnore(const std::string& dir_name, const std::string& content) {
        Write(dir_name.empty() ? ".gitignore" : dir_name + "/.gitignore", content);
    }

    std::vector<std::string> Lines(const tools::ToolResult& result) const {
        std::vector<std::string> lines;
        std::size_t start = 0;
        while (start < result.content_text.size()) {
            const auto nl = result.content_text.find('\n', start);
            if (nl == std::string::npos) {
                lines.push_back(result.content_text.substr(start));
                break;
            }
            lines.push_back(result.content_text.substr(start, nl - start));
            start = nl + 1;
        }
        return lines;
    }
};

}  // namespace

// --- glob matcher ------------------------------------------------------------

TEST_CASE("glob translation: single-segment patterns stay within a segment",
          "[tools_search][glob_matcher]") {
    CHECK(Matches(*tools_search::GlobToRegex("*.cpp", GlobFlags::kNone), "main.cpp"));
    CHECK_FALSE(Matches(*tools_search::GlobToRegex("*.cpp", GlobFlags::kNone), "src/main.cpp"));
    CHECK(Matches(*tools_search::GlobToRegex("a?c", GlobFlags::kNone), "abc"));
    CHECK_FALSE(Matches(*tools_search::GlobToRegex("a?c", GlobFlags::kNone), "a/c"));
}

TEST_CASE("glob translation: ** spans directories in its three placements",
          "[tools_search][glob_matcher]") {
    // Leading: zero or more directories.
    CHECK(Matches(*tools_search::GlobToRegex("**/b", GlobFlags::kNone), "b"));
    CHECK(Matches(*tools_search::GlobToRegex("**/b", GlobFlags::kNone), "x/b"));
    CHECK(Matches(*tools_search::GlobToRegex("**/b", GlobFlags::kNone), "x/y/b"));
    // Trailing: everything beneath, never the directory itself.
    CHECK(Matches(*tools_search::GlobToRegex("a/**", GlobFlags::kNone), "a/b"));
    CHECK(Matches(*tools_search::GlobToRegex("a/**", GlobFlags::kNone), "a/b/c"));
    CHECK_FALSE(Matches(*tools_search::GlobToRegex("a/**", GlobFlags::kNone), "a"));
    // Middle: the preceding slash was already emitted literally.
    CHECK(Matches(*tools_search::GlobToRegex("a/**/c", GlobFlags::kNone), "a/c"));
    CHECK(Matches(*tools_search::GlobToRegex("a/**/c", GlobFlags::kNone), "a/b/c"));
}

TEST_CASE("glob translation: braces alternate only when asked",
          "[tools_search][glob_matcher]") {
    CHECK(Matches(*tools_search::GlobToRegex("{a,b}.txt", GlobFlags::kBraceAlternation), "a.txt"));
    CHECK(Matches(*tools_search::GlobToRegex("{a,b}.txt", GlobFlags::kBraceAlternation), "b.txt"));
    CHECK_FALSE(Matches(*tools_search::GlobToRegex("{a,b}.txt", GlobFlags::kBraceAlternation), "c.txt"));
    // gitignore semantics: braces are literal characters.
    CHECK(Matches(*tools_search::GlobToRegex("{a}", GlobFlags::kNone), "{a}"));
    CHECK_FALSE(Matches(*tools_search::GlobToRegex("{a}", GlobFlags::kBraceAlternation), "{a}"));
}

TEST_CASE("glob translation: unterminated constructs fail the pattern",
          "[tools_search][glob_matcher]") {
    CHECK_FALSE(tools_search::GlobToRegex("a[b", GlobFlags::kNone).has_value());
    CHECK_FALSE(tools_search::GlobToRegex("{a,b", GlobFlags::kBraceAlternation).has_value());
    CHECK_FALSE(tools_search::GlobToRegex("a}", GlobFlags::kBraceAlternation).has_value());
}

TEST_CASE("glob translation: character classes and escapes",
          "[tools_search][glob_matcher]") {
    CHECK(Matches(*tools_search::GlobToRegex("[a-c].h", GlobFlags::kNone), "b.h"));
    CHECK_FALSE(Matches(*tools_search::GlobToRegex("[a-c].h", GlobFlags::kNone), "d.h"));
    CHECK(Matches(*tools_search::GlobToRegex("[!a].h", GlobFlags::kNone), "b.h"));
    CHECK_FALSE(Matches(*tools_search::GlobToRegex("[!a].h", GlobFlags::kNone), "a.h"));
    CHECK(Matches(*tools_search::GlobToRegex(R"(a\*.txt)", GlobFlags::kNone), "a*.txt"));
    CHECK(Matches(*tools_search::GlobToRegex("x.+", GlobFlags::kNone), "x.+"));  // regex meta escaped
    CHECK(tools_search::IsLiteralGlob("README"));
    CHECK_FALSE(tools_search::IsLiteralGlob("*.md"));
}

TEST_CASE("glob translation: backslash escapes work inside character classes",
          "[tools_search][glob_matcher][invariant]") {
    // `\]` inside a class is a literal member, not the terminator: the old
    // scan took the first `]` as the close, truncating the class (and
    // mangling everything after it).
    CHECK(Matches(*tools_search::GlobToRegex(R"([\]a].h)", GlobFlags::kNone), "].h"));
    CHECK(Matches(*tools_search::GlobToRegex(R"([\]a].h)", GlobFlags::kNone), "a.h"));
    CHECK_FALSE(Matches(*tools_search::GlobToRegex(R"([\]a].h)", GlobFlags::kNone), "b.h"));
    // POSIX first-position literals still hold.
    CHECK(Matches(*tools_search::GlobToRegex(R"([a\]b].h)", GlobFlags::kNone), "b.h"));
    // And a plain unterminated class still fails.
    CHECK_FALSE(tools_search::GlobToRegex(R"([a\])", GlobFlags::kNone).has_value());
}

// --- gitignore rules ---------------------------------------------------------

TEST_CASE("gitignore: basename patterns match at any depth", "[tools_search][gitignore]") {
    const auto set = tools_search::GitignoreSet::Parse("*.log\nbuild\n");
    CHECK(set.Match("a.log", false) == tools_search::GitignoreSet::Verdict::kIgnore);
    CHECK(set.Match("sub/a.log", false) == tools_search::GitignoreSet::Verdict::kIgnore);
    CHECK(set.Match("build", true) == tools_search::GitignoreSet::Verdict::kIgnore);
    CHECK(set.Match("sub/build", true) == tools_search::GitignoreSet::Verdict::kIgnore);
    CHECK_FALSE(set.Match("a.txt", false).has_value());
    // Basename `build` as a FILE: the rule matches it too (git semantics).
    CHECK(set.Match("x/build", false) == tools_search::GitignoreSet::Verdict::kIgnore);
}

TEST_CASE("gitignore: anchoring, dir-only and comments", "[tools_search][gitignore]") {
    const auto set = tools_search::GitignoreSet::Parse(
        "# comment\n/root-only\n/dir-only/\nsub/pair\n\\#literal\n\\!kept\n");
    CHECK(set.Match("root-only", false) == tools_search::GitignoreSet::Verdict::kIgnore);
    CHECK_FALSE(set.Match("x/root-only", false).has_value());  // anchored
    CHECK(set.Match("dir-only", true) == tools_search::GitignoreSet::Verdict::kIgnore);
    CHECK_FALSE(set.Match("dir-only", false).has_value());     // dir-only rule, file
    CHECK(set.Match("sub/pair", false) == tools_search::GitignoreSet::Verdict::kIgnore);
    CHECK_FALSE(set.Match("other/pair", false).has_value());   // interior slash anchors
    CHECK(set.Match("#literal", false) == tools_search::GitignoreSet::Verdict::kIgnore);
    CHECK(set.Match("!kept", false) == tools_search::GitignoreSet::Verdict::kIgnore);
}

TEST_CASE("gitignore: the last matching rule in a set decides (negation)",
          "[tools_search][gitignore]") {
    const auto set = tools_search::GitignoreSet::Parse("*.log\n!keep.log\n");
    CHECK(set.Match("a.log", false) == tools_search::GitignoreSet::Verdict::kIgnore);
    CHECK(set.Match("keep.log", false) == tools_search::GitignoreSet::Verdict::kKeep);
    // Order matters: negation first, pattern after, means still ignored.
    const auto reversed = tools_search::GitignoreSet::Parse("!keep.log\n*.log\n");
    CHECK(reversed.Match("keep.log", false) == tools_search::GitignoreSet::Verdict::kIgnore);
}

TEST_CASE("gitignore: deeper sets override shallower ones", "[tools_search][gitignore]") {
    tools_search::GitignoreStack stack;
    stack.Push(0, tools_search::GitignoreSet::Parse("*.log\n!root.log\n"));
    stack.Push(1, tools_search::GitignoreSet::Parse("!*.log\n"));
    // The depth-1 set re-includes every .log under it, overriding root.
    CHECK(stack.IsIgnored("sub/a.log", false) == false);
    CHECK(stack.IsIgnored("a.log", false) == true);  // root's own verdicts unaffected

    tools_search::GitignoreStack one_set;
    one_set.Push(0, tools_search::GitignoreSet::Parse("gen/**\n"));
    CHECK(one_set.IsIgnored("gen/x/y", false) == true);
    CHECK(one_set.IsIgnored("gen/x", true) == true);
    CHECK(one_set.IsIgnored("gen", true) == false);  // `gen/**` does not match gen itself
}

// --- tree walker ---------------------------------------------------------------

TEST_CASE("walker: deterministic order, always-skipped VCS dirs, file stats",
          "[tools_search][walker]") {
    Fixture f;
    f.Write("b.txt", "x");
    f.Write("a.txt", "x");
    f.Write(".git/config", "x");
    f.Write("sub/c.txt", "x");
    f.Write(".hg/store", "x");

    std::vector<std::string> rels;
    tools_search::WalkOptions options;
    const auto stats = tools_search::WalkTree(f.dir.path(), options,
                                              [&](const tools_search::WalkEntry& e) {
                                                  rels.push_back(e.rel_path);
                                                  return true;
                                              });
    REQUIRE(rels == std::vector<std::string>{"a.txt", "b.txt", "sub", "sub/c.txt"});
    CHECK(stats.files_seen == 3);
    CHECK(stats.dirs_seen == 1);
    CHECK_FALSE(stats.timed_out);
    CHECK_FALSE(stats.cancelled);
    CHECK_FALSE(stats.limit_stopped);
}

TEST_CASE("walker: applies gitignore files at every level", "[tools_search][walker]") {
    Fixture f;
    f.Write(".gitignore", "*.log\n/generated/\n");
    f.Write("keep.txt", "x");
    f.Write("no.log", "x");
    f.Write("generated/out.txt", "x");
    f.Write("sub/.gitignore", "*.log\n!special.log\n");
    f.Write("sub/special.log", "x");
    f.Write("sub/other.log", "x");

    std::vector<std::string> rels;
    tools_search::WalkOptions options;
    tools_search::WalkTree(f.dir.path(), options,
                           [&](const tools_search::WalkEntry& e) {
                               rels.push_back(e.rel_path);
                               return true;
                           });
    // Nested negation re-includes special.log only under sub/.
    REQUIRE(rels == std::vector<std::string>{".gitignore", "keep.txt", "sub",
                                             "sub/.gitignore", "sub/special.log"});
}

TEST_CASE("walker: prunes ignored directories and never follows symlinks",
          "[tools_search][walker]") {
    Fixture f;
    f.Write(".gitignore", "ignored/\n");
    f.Write("ignored/deep/file.txt", "x");  // nothing under an ignored dir survives
    f.Write("real/target.txt", "x");
    f.Write("link-dir", "placeholder");  // becomes a symlink below
    f.Write("link-file", "placeholder");

    std::filesystem::remove(f.dir.path() / "link-dir");
    std::filesystem::remove(f.dir.path() / "link-file");
    std::filesystem::create_directory_symlink(f.dir.path() / "real", f.dir.path() / "link-dir");
    std::filesystem::create_symlink(f.dir.path() / "real" / "target.txt",
                                    f.dir.path() / "link-file");

    std::vector<std::string> rels;
    tools_search::WalkOptions options;
    tools_search::WalkTree(f.dir.path(), options,
                           [&](const tools_search::WalkEntry& e) {
                               rels.push_back(e.rel_path);
                               return true;
                           });
    REQUIRE(rels == std::vector<std::string>{".gitignore", "real", "real/target.txt"});
}

TEST_CASE("walker: oversize files are skipped and counted", "[tools_search][walker]") {
    Fixture f;
    f.Write("big.txt", std::string(3000, 'x'));
    f.Write("small.txt", "x");
    tools_search::WalkOptions options;
    options.max_file_bytes = 1024;
    std::vector<std::string> rels;
    const auto stats = tools_search::WalkTree(f.dir.path(), options,
                                              [&](const tools_search::WalkEntry& e) {
                                                  rels.push_back(e.rel_path);
                                                  return true;
                                              });
    REQUIRE(rels == std::vector<std::string>{"small.txt"});
    CHECK(stats.skipped_oversize == 1);
}

TEST_CASE("walker: cancellation and the callback's own limit stop the walk",
          "[tools_search][walker]") {
    Fixture f;
    for (int i = 0; i < 50; ++i) f.Write("f" + std::to_string(i) + ".txt", "x");

    tools_search::WalkOptions options;
    int calls = 0;
    options.is_cancelled = [&calls] { return ++calls > 10; };
    const auto stats = tools_search::WalkTree(f.dir.path(), options,
                                              [](const tools_search::WalkEntry&) { return true; });
    CHECK(stats.cancelled);
    CHECK(stats.files_seen < 50);

    tools_search::WalkOptions limit_options;
    int seen = 0;
    const auto limit_stats = tools_search::WalkTree(
        f.dir.path(), limit_options, [&seen](const tools_search::WalkEntry&) {
            return ++seen < 5;
        });
    CHECK(limit_stats.limit_stopped);
    CHECK(limit_stats.files_seen == 4);
}

TEST_CASE("walker: timeout stops the walk and is reported", "[tools_search][walker]") {
    Fixture f;
    for (int i = 0; i < 4000; ++i) {
        f.Write("d" + std::to_string(i % 20) + "/f" + std::to_string(i) + ".txt", "x");
    }
    tools_search::WalkOptions options;
    options.timeout = std::chrono::milliseconds(1);
    const auto stats = tools_search::WalkTree(f.dir.path(), options,
                                              [](const tools_search::WalkEntry&) { return true; });
    CHECK(stats.timed_out);
}

// --- glob tool -----------------------------------------------------------------

TEST_CASE("glob: recursive pattern, mtime order, literal fast path",
          "[tools_search][glob_tool]") {
    Fixture f;
    f.Write("src/old.cpp", "x");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    f.Write("src/new.cpp", "x");
    auto tool = tools_search::MakeGlobTool(f.settings);
    auto ctx = f.Context();

    const auto result = tool->execute({{"pattern", "src/**/*.cpp"}}, ctx);
    CHECK_FALSE(result.is_error);
    // Newest first.
    CHECK(result.content_text.find("src/new.cpp") < result.content_text.find("src/old.cpp"));

    // Literal: no metacharacters, no walk needed.
    const auto literal = tool->execute({{"pattern", "src/new.cpp"}}, ctx);
    CHECK_FALSE(literal.is_error);
    CHECK(literal.content_text == "src/new.cpp\n");

    const auto missing = tool->execute({{"pattern", "src/nope.cpp"}}, ctx);
    CHECK_FALSE(missing.is_error);
    CHECK(missing.content_text == "No files found");
}

TEST_CASE("glob: head_limit truncates with a footer stating the total",
          "[tools_search][glob_tool]") {
    Fixture f;
    for (int i = 0; i < 10; ++i) f.Write("f" + std::to_string(i) + ".txt", "x");
    auto tool = tools_search::MakeGlobTool(f.settings);
    auto ctx = f.Context();

    const auto result = tool->execute({{"pattern", "*.txt"}, {"head_limit", 3}}, ctx);
    CHECK_FALSE(result.is_error);
    CHECK(result.content_text.find("(10 total; showing newest 3)") != std::string::npos);
}

TEST_CASE("glob: invalid pattern is an error, not a silent empty result",
          "[tools_search][glob_tool]") {
    Fixture f;
    auto tool = tools_search::MakeGlobTool(f.settings);
    auto ctx = f.Context();
    CHECK(tool->execute({{"pattern", "a[b"}}, ctx).is_error);
    CHECK(tool->execute({{"pattern", ""}}, ctx).is_error);
    CHECK(tool->execute({{"pattern", 7}}, ctx).is_error);
}

TEST_CASE("glob: the literal fast path is confined to the search root",
          "[tools_search][glob_tool]") {
    // The fast path joins the pattern straight onto the confined root, so an
    // absolute pattern would replace the root and a ".." component would
    // escape it -- an existence oracle outside project_root from a ReadOnly
    // (pre-allowed) tool. Both must be rejected before the join.
    Fixture f;
    f.Write("src/main.cpp", "x");
    auto tool = tools_search::MakeGlobTool(f.settings);
    auto ctx = f.Context();

    const auto absolute = tool->execute({{"pattern", "/etc/passwd"}}, ctx);
    CHECK(absolute.is_error);
    CHECK(absolute.content_text.find("etc/passwd") == std::string::npos);

    CHECK(tool->execute({{"pattern", "../../etc"}}, ctx).is_error);

    // An ordinary literal inside the root still takes the fast path.
    const auto literal = tool->execute({{"pattern", "src/main.cpp"}}, ctx);
    CHECK_FALSE(literal.is_error);
    CHECK(literal.content_text == "src/main.cpp\n");
}

TEST_CASE("glob: a search root below the project root prefixes its paths",
          "[tools_search][glob_tool]") {
    Fixture f;
    f.Write("src/old.cpp", "x");
    f.Write("src/nested/new.cpp", "x");
    f.Write("other/elsewhere.cpp", "x");
    auto tool = tools_search::MakeGlobTool(f.settings);
    auto ctx = f.Context();

    const auto result = tool->execute({{"pattern", "**/*.cpp"}, {"path", "src"}}, ctx);
    CHECK_FALSE(result.is_error);
    // Project-root-relative, NOT walk-root-relative: these are literal read
    // arguments -- the model pastes what it sees. (Negatives are line-anchored:
    // "nested/new.cpp" alone is a substring of the prefixed path.)
    CHECK(result.content_text.find("src/nested/new.cpp") != std::string::npos);
    CHECK(result.content_text.find("src/old.cpp") != std::string::npos);
    CHECK(result.content_text.find("\nnested/new.cpp\n") == std::string::npos);
    CHECK(result.content_text.find("\nold.cpp\n") == std::string::npos);
    CHECK(result.content_text.find("elsewhere.cpp") == std::string::npos);
}

TEST_CASE("glob: literal fast path with a sub-root search root is prefixed too",
          "[tools_search][glob_tool]") {
    Fixture f;
    f.Write("src/new.cpp", "x");
    auto tool = tools_search::MakeGlobTool(f.settings);
    auto ctx = f.Context();

    const auto result = tool->execute({{"pattern", "new.cpp"}, {"path", "src"}}, ctx);
    CHECK_FALSE(result.is_error);
    CHECK(result.content_text == "src/new.cpp\n");
}

// --- grep tool -----------------------------------------------------------------

TEST_CASE("grep: files_with_matches default mode with gitignore respected",
          "[tools_search][grep_tool]") {
    Fixture f;
    f.Write(".gitignore", "skip/\n");
    f.Write("a.py", "alpha\n");
    f.Write("b.py", "beta\n");
    f.Write("c.md", "alpha beta\n");
    f.Write("skip/d.py", "alpha\n");

    auto tool = tools_search::MakeGrepTool(f.settings);
    auto ctx = f.Context();
    const auto result = tool->execute({{"pattern", "alpha"}, {"glob", "*.py"}}, ctx);
    CHECK_FALSE(result.is_error);
    CHECK(result.content_text.find("a.py") != std::string::npos);
    CHECK(result.content_text.find("c.md") == std::string::npos);
    CHECK(result.content_text.find("skip/d.py") == std::string::npos);
}

TEST_CASE("grep: content mode with context and separators", "[tools_search][grep_tool]") {
    Fixture f;
    f.Write("code.py", "one\ntwo\nmatch\ntwo\none\nmatch2\nend\n");
    auto tool = tools_search::MakeGrepTool(f.settings);
    auto ctx = f.Context();

    const auto result = tool->execute(
        {{"pattern", "match"}, {"output_mode", "content"}, {"context_before", 1},
         {"context_after", 1}},
        ctx);
    CHECK_FALSE(result.is_error);
    const auto lines = f.Lines(result);
    // Matches at lines 3 and 6 with one line of context each produce adjacent
    // blocks [2,4] and [5,7] -- a zero-line gap, so they merge into one block
    // [2,7] with no separator, like ripgrep.
    REQUIRE(lines.size() == 6);
    CHECK(lines[0] == "code.py-2:two");
    CHECK(lines[1] == "code.py:3:match");
    CHECK(lines[2] == "code.py-4:two");
    CHECK(lines[3] == "code.py-5:one");
    CHECK(lines[4] == "code.py:6:match2");
    CHECK(lines[5] == "code.py-7:end");
}

TEST_CASE("grep: content mode separates blocks that have a gap", "[tools_search][grep_tool]") {
    Fixture f;
    f.Write("code.py", "match\n\n\n\n\nmatch\n");
    auto tool = tools_search::MakeGrepTool(f.settings);
    auto ctx = f.Context();

    const auto result = tool->execute(
        {{"pattern", "match"}, {"output_mode", "content"}, {"context_before", 1},
         {"context_after", 1}},
        ctx);
    const auto lines = f.Lines(result);
    // Blocks [1,2] and [5,6]: a three-line gap emits ripgrep's `--` separator.
    REQUIRE(lines.size() == 5);
    CHECK(lines[0] == "code.py:1:match");
    CHECK(lines[1] == "code.py-2:");
    CHECK(lines[2] == "--");
    CHECK(lines[3] == "code.py-5:");
    CHECK(lines[4] == "code.py:6:match");
}

TEST_CASE("grep: count mode", "[tools_search][grep_tool]") {
    Fixture f;
    f.Write("a.txt", "hit\nmiss\nhit\n");
    f.Write("b.txt", "miss\n");
    auto tool = tools_search::MakeGrepTool(f.settings);
    auto ctx = f.Context();

    const auto result = tool->execute({{"pattern", "hit"}, {"output_mode", "count"}}, ctx);
    CHECK_FALSE(result.is_error);
    CHECK(result.content_text == "a.txt:2\n");
}

TEST_CASE("grep: paths are project-root-relative whatever the search root",
          "[tools_search][grep_tool]") {
    Fixture f;
    f.Write("sub/a.py", "alpha\nmore\nalpha\n");
    f.Write("sub/code.py", "one\nmatch\n");
    f.Write("other/b.py", "alpha\n");
    f.Write("sub/solo.txt", "hit\nmiss\n");
    auto tool = tools_search::MakeGrepTool(f.settings);
    auto ctx = f.Context();

    // files_with_matches mode.
    const auto files = tool->execute({{"pattern", "alpha"}, {"path", "sub"}}, ctx);
    CHECK_FALSE(files.is_error);
    CHECK(files.content_text.find("sub/a.py") != std::string::npos);
    CHECK(files.content_text.find("\na.py:") == std::string::npos);  // not walk-root-relative
    CHECK(files.content_text.find("b.py") == std::string::npos);    // not under the search root

    // content mode: the file header carries the prefix too.
    const auto content = tool->execute(
        {{"pattern", "match"}, {"path", "sub"}, {"output_mode", "content"}}, ctx);
    CHECK_FALSE(content.is_error);
    CHECK(content.content_text.find("sub/code.py:2:match") != std::string::npos);

    // count mode.
    const auto count = tool->execute(
        {{"pattern", "alpha"}, {"path", "sub"}, {"output_mode", "count"}}, ctx);
    CHECK_FALSE(count.is_error);
    CHECK(count.content_text == "sub/a.py:2\n");

    // A FILE as the search root: the walker reports it under its bare
    // filename, so the prefix must be the file's PARENT -- "sub/solo.txt",
    // never "sub/solo.txt/solo.txt".
    const auto file_root = tool->execute(
        {{"pattern", "hit"}, {"path", "sub/solo.txt"}, {"output_mode", "content"}}, ctx);
    CHECK_FALSE(file_root.is_error);
    CHECK(file_root.content_text.find("sub/solo.txt:1:hit") != std::string::npos);
    CHECK(file_root.content_text.find("sub/solo.txt/") == std::string::npos);
}

TEST_CASE("grep: case insensitivity and binary/oversize skips",
          "[tools_search][grep_tool]") {
    Fixture f;
    f.settings.grep_max_file_bytes = 1024;  // the test's "huge" file is over this
    f.Write("Mixed.txt", "Hello\n");
    f.Write("bin.dat", std::string("ab\0cd", 5));
    f.Write("huge.txt", std::string(4000, 'x') + "needle");
    auto tool = tools_search::MakeGrepTool(f.settings);
    auto ctx = f.Context();

    const auto insensitive =
        tool->execute({{"pattern", "hello"}, {"case_insensitive", true}}, ctx);
    CHECK_FALSE(insensitive.is_error);
    CHECK(insensitive.content_text.find("Mixed.txt") != std::string::npos);

    const auto sensitive = tool->execute({{"pattern", "hello"}}, ctx);
    // Nothing matched -- but the oversize skip is still disclosed; a bare
    // "No matches found" over a partial walk would be a false negative.
    CHECK(sensitive.content_text.find("Mixed.txt") == std::string::npos);
    CHECK(sensitive.content_text.find("skipped: larger than") != std::string::npos);

    // Binary file skipped entirely; oversize file skipped and disclosed.
    const auto result = tool->execute({{"pattern", "needle"}}, ctx);
    CHECK_FALSE(result.is_error);
    CHECK(result.content_text.find("bin.dat") == std::string::npos);
    CHECK(result.content_text.find("skipped: larger than") != std::string::npos);
}

TEST_CASE("grep: head_limit truncates with a footer and a re-run hint",
          "[tools_search][grep_tool]") {
    Fixture f;
    for (int i = 0; i < 10; ++i) f.Write("f" + std::to_string(i) + ".txt", "needle\n");
    auto tool = tools_search::MakeGrepTool(f.settings);
    auto ctx = f.Context();

    const auto result = tool->execute({{"pattern", "needle"}, {"head_limit", 4}}, ctx);
    CHECK_FALSE(result.is_error);
    const auto lines = f.Lines(result);
    // The walk stops at the budget, so the total is an honest lower bound.
    REQUIRE(lines.size() == 5);
    CHECK(lines.back() ==
          "(showing 4 of at least 5 files with matches; re-run with offset=4 for more)");

    // Offset pages forward.
    const auto page2 = tool->execute(
        {{"pattern", "needle"}, {"head_limit", 4}, {"offset", 4}}, ctx);
    CHECK(page2.content_text.find("f4.txt") != std::string::npos);
    CHECK(page2.content_text.find("f3.txt") == std::string::npos);
}

TEST_CASE("grep: RE2 rejects pathological patterns in linear time -- invalid ones error",
          "[tools_search][grep_tool]") {
    Fixture f;
    auto tool = tools_search::MakeGrepTool(f.settings);
    auto ctx = f.Context();
    const auto result = tool->execute({{"pattern", "(unclosed"}}, ctx);
    CHECK(result.is_error);
    CHECK(result.content_text.find("invalid regex") != std::string::npos);
}

TEST_CASE("grep: model-authored wrong types are errors, not crashes",
          "[tools_search][grep_tool]") {
    Fixture f;
    f.Write("a.txt", "x\n");
    auto tool = tools_search::MakeGrepTool(f.settings);
    auto ctx = f.Context();

    CHECK(tool->execute({{"pattern", "x"}, {"output_mode", 3}}, ctx).is_error);
    CHECK(tool->execute({{"pattern", "x"}, {"case_insensitive", "yes"}}, ctx).is_error);
    CHECK(tool->execute({{"pattern", "x"}, {"head_limit", "ten"}}, ctx).is_error);
    CHECK(tool->execute({{"pattern", "x"}, {"context_before", 2}}, ctx).is_error);  // needs content mode
    CHECK(tool->execute({{"pattern", "x"}, {"path", 1}}, ctx).is_error);
}

TEST_CASE("grep: cancellation mid-walk returns partial results marked as such",
          "[tools_search][grep_tool]") {
    Fixture f;
    for (int i = 0; i < 200; ++i) f.Write("f" + std::to_string(i) + ".txt", "needle\n");
    auto tool = tools_search::MakeGrepTool(f.settings);
    auto ctx = f.Context();
    int polls = 0;
    ctx.is_cancelled = [&polls] { return ++polls > 20; };

    const auto result = tool->execute({{"pattern", "needle"}}, ctx);
    CHECK_FALSE(result.is_error);
    CHECK(result.content_text.find("(stopped early: cancelled)") != std::string::npos);
    CHECK(result.content_text.find("at least") != std::string::npos);
}

TEST_CASE("grep: a file path as the search root searches that file",
          "[tools_search][grep_tool]") {
    Fixture f;
    f.Write("solo.txt", "hit\nmiss\n");
    auto tool = tools_search::MakeGrepTool(f.settings);
    auto ctx = f.Context();

    const auto result = tool->execute(
        {{"pattern", "hit"}, {"path", "solo.txt"}, {"output_mode", "content"}}, ctx);
    CHECK_FALSE(result.is_error);
    CHECK(result.content_text.find("solo.txt:1:hit") != std::string::npos);
}

TEST_CASE("grep: streams output through the tool context", "[tools_search][grep_tool]") {
    Fixture f;
    f.Write("a.txt", "hit\n");
    auto tool = tools_search::MakeGrepTool(f.settings);
    auto ctx = f.Context();
    std::string streamed;
    ctx.emit_output = [&](std::string_view chunk) { streamed.append(chunk); };

    (void)tool->execute({{"pattern", "hit"}, {"output_mode", "content"}}, ctx);
    CHECK(streamed.find("a.txt:1:hit\n") != std::string::npos);
}

// --- settings ------------------------------------------------------------------

TEST_CASE("search settings: absent or invalid values keep defaults",
          "[tools_search][settings]") {
    CHECK(tools_search::ParseSearchSettings(nlohmann::json::object()).grep_max_results == 1000);
    CHECK(tools_search::ParseSearchSettings(nlohmann::json{}).glob_default_limit == 200);

    const auto custom = tools_search::ParseSearchSettings(nlohmann::json{
        {"grep", {{"timeout_ms", 5000}, {"max_results", -3}}},
        {"glob", {{"default_limit", 42}}}});
    CHECK(custom.grep_timeout_ms == 5000);
    CHECK(custom.grep_max_results == 1000);  // non-positive falls back
    CHECK(custom.glob_default_limit == 42);
}

// --- executor integration ------------------------------------------------------

TEST_CASE("search tools declare their paths so the executor classifies them",
          "[tools_search][confinement]") {
    Fixture f;
    auto grep = tools_search::MakeGrepTool(f.settings);
    auto glob = tools_search::MakeGlobTool(f.settings);

    CHECK(grep->path_confinement_supported());
    CHECK(grep->ExtractPath({{"path", "src"}}) == std::optional<std::string>{"src"});
    CHECK(grep->ExtractPath({}) == std::nullopt);  // default root, nothing to classify
    CHECK(glob->path_confinement_supported());
    CHECK(glob->ExtractPath({{"pattern", "x"}, {"path", "src"}}) ==
          std::optional<std::string>{"src"});

    // Both are read-only: the permission substrate's default already allows
    // them, and the out-of-workdir fold is what asks when the search root
    // escapes the project.
    CHECK(grep->spec().category == tools::ToolCategory::ReadOnly);
    CHECK(glob->spec().category == tools::ToolCategory::ReadOnly);
}

TEST_CASE("brace expansion is translation-capped, however deeply the model nests",
          "[tools_search][invariant]") {
    // Nested `{a,b}` alternation used to DOUBLE the translated output per
    // level: a ~320-byte pattern reached a 2^40-byte string before RE2 ever
    // saw it, defeating the linear-time guarantee the dependency was added
    // for -- the blowup was in the TRANSLATOR, which the walk timeout does
    // not cover. Depth 16 and a 64 KiB translation make the refusal
    // unreachable-from-legitimate use and bounded everywhere else.
    std::string pattern;
    for (int i = 0; i < 40; ++i) pattern += "{{a,b},";
    pattern += "x";
    for (int i = 0; i < 40; ++i) pattern += "}";

    const auto started = std::chrono::steady_clock::now();
    const auto translated = GlobToRegex(pattern, GlobFlags::kBraceAlternation);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK_FALSE(translated.has_value());
    CHECK(elapsed < std::chrono::seconds(2));
}

// --- permission disclosure ----------------------------------------------------
//
// Regression: the base Tool::Disclose default named only the tool
// ("glob"/"grep"), never the pattern or search root -- both ReadOnly, so
// usually pre-allowed, but an out-of-workdir path or a stricter
// fallback.read_only still raises this prompt, and it should say what it's
// about to read.

TEST_CASE("glob discloses the pattern and search root", "[tools_search][glob_tool][disclosure]") {
    Fixture f;
    auto tool = tools_search::MakeGlobTool(f.settings);
    auto ctx = f.Context();
    const auto disclosure = tool->Disclose({{"pattern", "*.cpp"}, {"path", "src"}}, ctx);
    CHECK(disclosure.summary.find("*.cpp") != std::string::npos);
    CHECK(disclosure.summary.find("src") != std::string::npos);
}

TEST_CASE("glob's disclosure defaults the search root to '.'", "[tools_search][glob_tool][disclosure]") {
    Fixture f;
    auto tool = tools_search::MakeGlobTool(f.settings);
    auto ctx = f.Context();
    const auto disclosure = tool->Disclose({{"pattern", "*.cpp"}}, ctx);
    CHECK(disclosure.summary.find("*.cpp") != std::string::npos);
    CHECK(disclosure.summary.find(".") != std::string::npos);
}

TEST_CASE("grep discloses the pattern and search root", "[tools_search][grep_tool][disclosure]") {
    Fixture f;
    auto tool = tools_search::MakeGrepTool(f.settings);
    auto ctx = f.Context();
    const auto disclosure = tool->Disclose({{"pattern", "TODO"}, {"path", "src"}}, ctx);
    CHECK(disclosure.summary.find("TODO") != std::string::npos);
    CHECK(disclosure.summary.find("src") != std::string::npos);
}
