// What does permission pattern matching cost on the path a file walk takes?
//
// A grep/glob-style tool asks the policy once per walked file (the per-file
// `path_permitted` predicate calls PermissionPolicy::mode_for with that file's
// canonical path as the subject), so the per-call cost of mode_for against a
// realistic rule set, multiplied by the number of files, is what a walk pays.
// This reports the number so "is precompiling globs worth it" is answered by a
// measurement rather than a guess.
//
// Hidden by default (leading dot): it reports rather than asserts, and a timing
// threshold in CI would be a flake generator. Run with
//   ./minimal_tests "[.bench]"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "minimal/tools/pattern_match.hpp"
#include "minimal/tools/permission.hpp"

using namespace minimal::tools;

namespace {

PermissionPolicy BenchPolicy() {
    PermissionPolicy policy;
    policy.pattern_overrides = default_pattern_rules({"read", "grep", "glob"});
    // A realistic operator rule set on top of the built-in dotenv rules.
    std::vector<PermissionPatternRule> rules;
    for (const char* glob : {"**/node_modules/**", "**/.git/**", "**/*.pem", "**/*.key",
                             "**/secrets/**", "**/id_rsa*", "**/.ssh/**", "**/build/**/*.o",
                             "**/target/**", "**/*.sqlite", "**/credentials*", "**/.aws/**",
                             "/etc/**", "/usr/**", "**/vendor/**/test/**", "**/dist/**",
                             "**/*.log", "**/.cache/**", "**/coverage/**", "**/tmp/**"}) {
        rules.push_back({glob, PermissionMode::Ask});
    }
    for (const char* tool : {"read", "grep", "glob"}) {
        auto& slot = policy.pattern_overrides[tool];
        slot.insert(slot.end(), rules.begin(), rules.end());
    }
    return policy;
}

std::vector<std::string> Subjects(int count) {
    std::vector<std::string> out;
    out.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        out.push_back("/home/user/work/project/src/module" + std::to_string(i % 40) +
                      "/component" + std::to_string(i % 17) + "/file" + std::to_string(i) +
                      ".cpp");
    }
    return out;
}

}  // namespace

TEST_CASE("cost of mode_for against a realistic rule set, per walked file", "[.bench]") {
    const PermissionPolicy policy = BenchPolicy();
    const auto subjects = Subjects(10000);

    const auto started = std::chrono::steady_clock::now();
    std::size_t asks = 0;
    for (const auto& subject : subjects) {
        PatternSubjects s;
        s.whole = subject;
        if (policy.mode_for("grep", ToolCategory::ReadOnly, s, /*subject_is_path=*/true) ==
            PermissionMode::Ask) {
            ++asks;
        }
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const double total_ms = std::chrono::duration<double, std::milli>(elapsed).count();
    std::printf("mode_for: %zu files in %.2f ms (%.0f ns/file, %zu asked)\n", subjects.size(),
                total_ms, total_ms * 1e6 / static_cast<double>(subjects.size()), asks);
    SUCCEED();
}

TEST_CASE("cost of GlobMatch alone, literal vs wildcard", "[.bench]") {
    const std::string subject =
        "/home/user/work/project/src/module12/component5/some_long_file_name_here.cpp";
    constexpr int kIterations = 200000;
    for (const char* glob : {"/home/user/work/project/src/module12/component5/x.cpp",
                             "**/*.pem", "**/node_modules/**", "**/build/**/*.o"}) {
        const auto started = std::chrono::steady_clock::now();
        std::size_t hits = 0;
        for (int i = 0; i < kIterations; ++i) hits += GlobMatch(glob, subject, true) ? 1 : 0;
        const double ns = std::chrono::duration<double, std::nano>(
                              std::chrono::steady_clock::now() - started)
                              .count() /
                          kIterations;
        std::printf("GlobMatch %-52s %7.0f ns  (hits %zu)\n", glob, ns, hits);
    }
    SUCCEED();
}
