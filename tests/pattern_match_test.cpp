#include <catch2/catch_test_macros.hpp>

#include "minimal/tools/pattern_match.hpp"

using minimal::tools::GlobMatch;
using minimal::tools::GlobShaped;

TEST_CASE("GlobMatch: literal patterns match exactly", "[pattern_match]") {
    CHECK(GlobMatch("git", "git", false));
    CHECK_FALSE(GlobMatch("git", "git status", false));
    CHECK_FALSE(GlobMatch("git status", "git", false));
}

TEST_CASE("GlobMatch: command flavor -- '*' crosses slashes", "[pattern_match]") {
    // A command line is not a path: '/' is an ordinary character, so "git *"
    // covers "git status src/file.cpp".
    CHECK(GlobMatch("git *", "git status", false));
    CHECK(GlobMatch("git *", "git push --force origin main", false));
    CHECK(GlobMatch("git *", "git commit -m src/x.cpp", false));
    CHECK_FALSE(GlobMatch("git *", "gitx status", false));
    CHECK_FALSE(GlobMatch("git *", "git", false));
}

TEST_CASE("GlobMatch: path flavor -- '*' stays in one component, '**' crosses", "[pattern_match]") {
    CHECK(GlobMatch("**/.env", "/work/proj/.env", true));
    CHECK(GlobMatch("**/.env", "/work/proj/sub/deep/.env", true));
    CHECK_FALSE(GlobMatch("**/.env", "/work/proj/.env.local", true));   // the suffix rule needs '.env.*'
    CHECK(GlobMatch("**/.env.*", "/work/proj/.env.local", true));
    CHECK(GlobMatch("/tmp/builds/*", "/tmp/builds/a", true));
    // '*' must not cross '/' in the path flavor...
    CHECK_FALSE(GlobMatch("/tmp/builds/*", "/tmp/builds/a/b", true));
    // ...'**' must.
    CHECK(GlobMatch("/tmp/builds/**", "/tmp/builds/a/b/c", true));
    CHECK(GlobMatch("*.cpp", "main.cpp", true));
    CHECK_FALSE(GlobMatch("*.cpp", "src/main.cpp", true));
}

TEST_CASE("GlobMatch: '?' matches one non-slash character", "[pattern_match]") {
    CHECK(GlobMatch("git ??", "git st", false));
    CHECK_FALSE(GlobMatch("git ??", "git s", false));
    CHECK(GlobMatch("/tmp/a?", "/tmp/ab", true));
    CHECK_FALSE(GlobMatch("/tmp/a?", "/tmp/a/", true));
}

TEST_CASE("GlobMatch: escapes match literally; a trailing escape fails closed",
          "[pattern_match]") {
    // A literal filename that happens to contain wildcard characters.
    CHECK(GlobMatch(R"(weird\*.md)", "weird*.md", false));
    CHECK_FALSE(GlobMatch(R"(weird\*.md)", "weirdX.md", false));
    CHECK_FALSE(GlobMatch(R"(trailing\)", "trailing\\", false));
    CHECK_FALSE(GlobMatch(R"(trailing\)", "trailing", false));

    // An escaped '?' must match the literal character, never the '?'
    // wildcard -- a rule like {"q\?.csv": "allow"} means the one file named
    // "q?.csv", not "any single character after q". Regression for a bug
    // where the escape branch unescaped the character but the wildcard check
    // below it re-interpreted the result, so \? matched anything.
    CHECK(GlobMatch(R"(a\?c)", "a?c", false));
    CHECK_FALSE(GlobMatch(R"(a\?c)", "abc", false));
}

TEST_CASE("GlobMatch: a pattern matches the whole subject, never a substring",
          "[pattern_match][invariant]") {
    CHECK_FALSE(GlobMatch("status", "git status", false));
    CHECK_FALSE(GlobMatch("git", "git status", false));
    CHECK(GlobMatch("*status", "git status", false));
}

TEST_CASE("GlobShaped: wildcard-shaped vs literal", "[pattern_match]") {
    CHECK(GlobShaped("git *"));
    CHECK(GlobShaped("**/.env"));
    CHECK(GlobShaped("a?c"));
    CHECK_FALSE(GlobShaped("git status"));
    CHECK_FALSE(GlobShaped("/tmp/builds"));
}
