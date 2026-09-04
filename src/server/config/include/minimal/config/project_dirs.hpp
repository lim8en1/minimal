#pragma once

// Ordered project directories for project-local discovery -- agents/, skills/,
// commands/ -- nearest first: the session's project root, then its ancestors up
// to AND INCLUDING the enclosing git worktree root. The .git boundary is what
// keeps a session created in a scratch subdirectory from picking up agents a
// user happens to keep in $HOME: without a git root there is no project scope
// beyond the root itself. A .git that is a FILE (a linked worktree) counts the
// same as a directory.
//
// Like project_context's ancestor walk, the root is canonicalized first so a
// symlinked project_root does not make the search start somewhere the root does
// not actually sit; failure keeps the path as given. An empty project_root
// yields an empty list -- "no project scope", the legal state for test fakes.

#include <filesystem>
#include <vector>

namespace minimal::config {

class ProjectDirs {
public:
    explicit ProjectDirs(const std::filesystem::path& project_root);

    // Nearest first: dirs()[0] is the canonical project root, the last entry is
    // the git worktree root (when one exists). Never contains duplicates.
    const std::vector<std::filesystem::path>& dirs() const { return dirs_; }

private:
    std::vector<std::filesystem::path> dirs_;
};

}  // namespace minimal::config
