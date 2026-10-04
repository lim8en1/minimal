#pragma once

// Which project directories the operator has trusted to supply their own agents,
// skills and commands (project_local.mode, `/trust-directory`).
//
// A repository's agents/, skills/ and commands/ directories are content its
// author chose, not the operator. Under the default mode (kTrusted) they are
// ignored until the operator trusts the directory; "all" restores the old
// load-everything behaviour and "none" never loads them.
//
// The unit of trust is the project's git worktree root (ProjectDirs finds it),
// so one grant covers a repository and every subdirectory a session starts in. A
// directory with no git root is its own root.
//
// State lives in one operator-owned file (not settings.json, which stays
// hand-authored). A missing file is "nothing trusted"; so is an unreadable or
// malformed one -- the fail-closed direction -- and the problem is logged.
//
// Thread-safe. generation() changes on every grant/revoke, so caches that
// depend on what is loadable can tell when to rebuild.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "minimal/config/settings_config.hpp"

namespace minimal::config {

// What a project directory would contribute if it were loaded: counts of the
// project-local agents, skills and commands found by the same walk discovery
// uses (the session's root up to its git root). Read-only; used to show the
// operator what a /trust-directory grant enables, and to say what is being
// ignored when nothing is trusted.
struct ProjectLocalInventory {
    std::size_t agents = 0;
    std::size_t skills = 0;
    std::size_t commands = 0;
    bool empty() const { return agents + skills + commands == 0; }
    // "2 agents, 1 skill, 3 commands" (zero counts omitted); "nothing" when empty.
    std::string Describe() const;
};
ProjectLocalInventory InspectProjectLocal(const std::filesystem::path& project_root);

class ProjectTrust {
public:
    ProjectTrust(std::filesystem::path state_file, ProjectLocalMode mode);

    ProjectLocalMode mode() const { return mode_; }

    // The directory a grant for `project_root` applies to: its git worktree root,
    // else the canonical root itself. Empty for an empty `project_root`.
    static std::filesystem::path RootFor(const std::filesystem::path& project_root);

    // May the project-local content under `project_root` load? Mode-aware: "all"
    // is always true, "none" always false, "trusted" asks IsTrusted. An empty
    // root has no project scope, so nothing to load.
    bool Allows(const std::filesystem::path& project_root) const;

    // An explicit grant covers `project_root` (regardless of mode).
    bool IsTrusted(const std::filesystem::path& project_root) const;

    // False with `error` set when the state file could not be written; the
    // in-memory set is changed only on success.
    bool Trust(const std::filesystem::path& project_root, std::string& error);
    // Revoking a directory that was never trusted succeeds (nothing to do).
    bool Revoke(const std::filesystem::path& project_root, std::string& error);

    std::vector<std::filesystem::path> trusted() const;
    std::uint64_t generation() const { return generation_.load(std::memory_order_acquire); }

private:
    bool Persist(const std::set<std::string>& roots, std::string& error) const;

    std::filesystem::path state_file_;
    ProjectLocalMode mode_;
    mutable std::mutex mutex_;
    std::set<std::string> roots_;  // canonical git-root paths, guarded by mutex_
    std::atomic<std::uint64_t> generation_{0};
};

}  // namespace minimal::config
