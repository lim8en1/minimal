#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <string>
#include <vector>

#include "minimal/config/project_dirs.hpp"
#include "temp_dir.hpp"

using namespace minimal;
using namespace minimal::config;

namespace {

// A throwaway directory the tests build project trees inside. Unique per
// process: catch_discover_tests runs every TEST_CASE as its own ctest entry.
struct TempProjectTree {
    testing::TempDir tempdir;
    std::filesystem::path dir = tempdir.path();

    std::filesystem::path Write(const std::filesystem::path& relative) const {
        const auto path = dir / relative;
        std::filesystem::create_directories(path.parent_path());
        { std::ofstream(path) << "x\n"; }
        return path;
    }

    // A .git DIRECTORY stands in for a checkout; the marker test accepts a file
    // too (worktrees), covered in its own case below.
    void MakeGitRepo(const std::filesystem::path& repo) const {
        Write(repo / ".git" / "HEAD");
    }
};

}  // namespace

TEST_CASE("an empty project root yields no project scope", "[project_dirs]") {
    const ProjectDirs dirs({});
    CHECK(dirs.dirs().empty());
}

TEST_CASE("the project root alone is searched when it is the git root", "[project_dirs]") {
    TempProjectTree tree;
    tree.MakeGitRepo("proj");

    const ProjectDirs dirs(tree.dir / "proj");
    REQUIRE(dirs.dirs().size() == 1);
    CHECK(dirs.dirs().front() == std::filesystem::weakly_canonical(tree.dir / "proj"));
}

TEST_CASE("ancestors are included up to and including the git root", "[project_dirs]") {
    TempProjectTree tree;
    tree.MakeGitRepo("proj");
    tree.Write("proj/deeply/nested/file.txt");

    const ProjectDirs dirs(tree.dir / "proj" / "deeply" / "nested");
    REQUIRE(dirs.dirs().size() == 3);
    CHECK(dirs.dirs()[0] == std::filesystem::weakly_canonical(tree.dir / "proj" / "deeply" / "nested"));
    CHECK(dirs.dirs()[1] == std::filesystem::weakly_canonical(tree.dir / "proj" / "deeply"));
    CHECK(dirs.dirs()[2] == std::filesystem::weakly_canonical(tree.dir / "proj"));
}

TEST_CASE("directories above the git root are never included", "[project_dirs]") {
    TempProjectTree tree;
    tree.Write("outside.txt");
    tree.MakeGitRepo("proj");
    tree.Write("proj/sub/file.txt");

    const ProjectDirs dirs(tree.dir / "proj" / "sub");
    for (const auto& dir : dirs.dirs()) {
        CHECK(dir != std::filesystem::weakly_canonical(tree.dir));
    }
}

TEST_CASE("a .git FILE marks the root, the way linked worktrees do", "[project_dirs]") {
    TempProjectTree tree;
    tree.Write("proj/.git");  // a file, not a directory
    tree.Write("proj/sub/file.txt");

    const ProjectDirs dirs(tree.dir / "proj" / "sub");
    REQUIRE(dirs.dirs().size() == 2);
    CHECK(dirs.dirs().back() == std::filesystem::weakly_canonical(tree.dir / "proj"));
}

TEST_CASE("without a git root only the project root itself is searched", "[project_dirs]") {
    TempProjectTree tree;
    // No .git anywhere in the tree: an ancestor walk with nothing to bound it
    // would leak $HOME or / into discovery, so the walk must not happen at all.
    tree.Write("plain/sub/file.txt");
    tree.Write("elsewhere/file.txt");

    const ProjectDirs dirs(tree.dir / "plain" / "sub");
    REQUIRE(dirs.dirs().size() == 1);
    CHECK(dirs.dirs().front() == std::filesystem::weakly_canonical(tree.dir / "plain" / "sub"));
}
