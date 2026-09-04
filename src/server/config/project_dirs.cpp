#include "minimal/config/project_dirs.hpp"

#include <system_error>

namespace minimal::config {
namespace {

// Worktrees and submodules keep .git as a file; a plain repo has a directory.
// exists() is the test that accepts both, and an unreadable parent reports
// cleanly false rather than throwing (error_code overload).
bool HasGitMarker(const std::filesystem::path& dir) {
    std::error_code ec;
    return std::filesystem::exists(dir / ".git", ec) && !ec;
}

}  // namespace

ProjectDirs::ProjectDirs(const std::filesystem::path& project_root) {
    if (project_root.empty()) return;

    // Same canonicalize-first stance as LoadProjectContext and path_confinement.
    std::error_code ec;
    std::filesystem::path origin = std::filesystem::weakly_canonical(project_root, ec);
    if (ec) origin = project_root;

    // Collect the walk speculatively and commit it only when a git root backs
    // it up: ancestors of a non-project directory (a checkout in /tmp with no
    // .git anywhere up to /) must not leak $HOME or /usr into discovery.
    std::vector<std::filesystem::path> walk{origin};
    if (HasGitMarker(origin)) {
        dirs_ = std::move(walk);
        return;
    }
    // The parent_path() == dir test stops the walk at the filesystem root,
    // where parent_path is a fixed point.
    for (std::filesystem::path dir = origin.parent_path();
         !dir.empty() && dir != dir.parent_path(); dir = dir.parent_path()) {
        walk.push_back(dir);
        if (HasGitMarker(dir)) {
            dirs_ = std::move(walk);
            return;
        }
    }
    // No git root anywhere up the chain: the project is the root itself.
    dirs_.push_back(origin);
}

}  // namespace minimal::config
