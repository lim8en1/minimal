#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <fstream>
#include <string>

#include <unistd.h>

#include "minimal/tools/path_confinement.hpp"

using namespace minimal::tools;

namespace {

// One enclosing arena so tests never touch anything in the OS temp dir besides
// this single directory; project/ and outside_sibling/ are siblings inside it,
// which is what lets ".." traversal reach "outside" without leaving the arena.
// Unique per fixture: ctest runs cases in parallel, and a shared arena means
// one case's remove_all() wiping another's tree mid-test.
struct TempProject {
    std::filesystem::path arena, root, outside;

    TempProject() {
        static std::atomic<int> counter{0};
        arena = std::filesystem::temp_directory_path() /
                ("minimal_path_confinement_test_arena-" + std::to_string(::getpid()) + "-" +
                 std::to_string(counter.fetch_add(1)));
        std::error_code ec;
        std::filesystem::remove_all(arena, ec);
        root = arena / "project";
        outside = arena / "outside_sibling";
        std::filesystem::create_directories(root / "subdir");
        std::filesystem::create_directories(outside);
        { std::ofstream(root / "inside.txt") << "hi"; }
        { std::ofstream(outside / "secret.txt") << "shh"; }
    }
    ~TempProject() {
        std::error_code ec;
        std::filesystem::remove_all(arena, ec);
    }
};

}  // namespace

TEST_CASE("confine resolves a path inside the root", "[path_confinement]") {
    TempProject project;
    auto result = confine(project.root, "inside.txt", /*must_exist=*/true);
    CHECK(result.ok);
    CHECK_FALSE(result.outside_project_root);
    CHECK(result.absolute_path == std::filesystem::canonical(project.root / "inside.txt"));
}

TEST_CASE("confine reports .. traversal as outside, not unsafe", "[path_confinement]") {
    TempProject project;
    auto result = confine(project.root, "../outside_sibling/secret.txt", /*must_exist=*/true);
    CHECK(result.ok);
    CHECK(result.outside_project_root);
}

TEST_CASE("confine reports an absolute escape as outside", "[path_confinement]") {
    TempProject project;
    auto result = confine(project.root, "/etc/hosts", /*must_exist=*/true);
    CHECK(result.ok);
    CHECK(result.outside_project_root);
}

TEST_CASE("confine follows a symlink that escapes the root", "[path_confinement]") {
    TempProject project;
    std::error_code ec;
    std::filesystem::create_symlink(project.outside, project.root / "escape_link", ec);
    REQUIRE_FALSE(ec);

    auto result = confine(project.root, "escape_link/secret.txt", /*must_exist=*/true);
    CHECK(result.ok);
    CHECK(result.outside_project_root);
}

TEST_CASE("confine fails closed on a symlink loop", "[path_confinement]") {
    TempProject project;
    std::error_code ec;
    std::filesystem::create_symlink(project.root / "loop_a", project.root / "loop_b", ec);
    REQUIRE_FALSE(ec);
    std::filesystem::create_symlink(project.root / "loop_b", project.root / "loop_a", ec);
    REQUIRE_FALSE(ec);

    auto result = confine(project.root, "loop_a", /*must_exist=*/true);
    CHECK_FALSE(result.ok);
    CHECK_FALSE(result.error.empty());
}

TEST_CASE("confine rejects embedded NUL outright", "[path_confinement]") {
    TempProject project;
    const std::string with_nul = std::string("inside") + '\0' + ".txt";
    auto result = confine(project.root, with_nul, /*must_exist=*/false);
    CHECK_FALSE(result.ok);
}

TEST_CASE("confine treats literal encoded-traversal text as an inert filename", "[path_confinement]") {
    // std::filesystem never URL-decodes, so "%2e%2e" is just a filename, not a
    // traversal attempt -- the encoded-traversal attack class is defeated by real
    // filesystem canonicalization, not by string blacklisting.
    TempProject project;
    auto result = confine(project.root, "%2e%2e/secret.txt", /*must_exist=*/false);
    CHECK(result.ok);
    CHECK_FALSE(result.outside_project_root);
}
