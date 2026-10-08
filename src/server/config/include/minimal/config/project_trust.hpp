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
// malformed one -- the fail-closed direction -- and the problem is logged. A
// grant or revoke re-reads the file under a cross-process lock, so servers
// sharing a config dir do not overwrite each other's grants.
//
// Thread-safe.

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "minimal/config/project_local_mode.hpp"

namespace minimal::config {

// One kind of project-local content and how much of it a project holds
// ("command"/"commands", 3).
struct ProjectContentCount {
    std::string singular;
    std::string plural;
    std::size_t count = 0;
};

// Whoever loads project-local content of its own (plugins: commands-md's
// commands/) reports it here, so the inventory below covers it without core
// knowing the layout. The plugin loader implements it. Must not throw.
class ProjectContentProvider {
public:
    virtual ~ProjectContentProvider() = default;
    virtual void DescribeProjectContent(const std::filesystem::path& project_root,
                                        std::vector<ProjectContentCount>& out) const = 0;
};

// What a project directory would contribute if it were loaded, found by the
// same walk discovery uses (the session's root up to its git root): core's
// own agents, plus whatever `plugins` reports (commands, skills, ...). Read-only; used to
// show the operator what a /trust-directory grant enables, and to say what is
// being ignored when nothing is trusted.
struct ProjectLocalInventory {
    std::vector<ProjectContentCount> items;  // same `singular` merged; zero counts kept out
    std::size_t CountOf(std::string_view singular) const;
    bool empty() const { return items.empty(); }
    // "2 agents, 1 skill, 3 commands" (zero counts omitted); "nothing" when empty.
    std::string Describe() const;
};
ProjectLocalInventory InspectProjectLocal(const std::filesystem::path& project_root,
                                          const ProjectContentProvider* plugins = nullptr);

// The question a plugin that loads project-local content asks before it
// does: may this root's own content load? Provided to plugins as the
// "core.project_trust" service.
inline constexpr const char* kProjectTrustServiceName = "core.project_trust";
class ProjectTrustQuery {
public:
    virtual ~ProjectTrustQuery() = default;
    virtual bool Allows(const std::filesystem::path& project_root) const = 0;
};

class ProjectTrust : public ProjectTrustQuery {
public:
    ProjectTrust(std::filesystem::path state_file, ProjectLocalMode mode);

    ProjectLocalMode mode() const { return mode_; }

    // The directory a grant for `project_root` applies to: its git worktree root,
    // else the canonical root itself. Empty for an empty `project_root`.
    static std::filesystem::path RootFor(const std::filesystem::path& project_root);

    // May the project-local content under `project_root` load? Mode-aware: "all"
    // is always true, "none" always false, "trusted" asks IsTrusted. An empty
    // root has no project scope, so nothing to load.
    bool Allows(const std::filesystem::path& project_root) const override;

    // An explicit grant covers `project_root` (regardless of mode).
    bool IsTrusted(const std::filesystem::path& project_root) const;

    // False with `error` set when the state file could not be locked or
    // written; the in-memory set is changed only on success (and then also
    // picks up grants other processes recorded).
    bool Trust(const std::filesystem::path& project_root, std::string& error);
    // Revoking a directory that was never trusted succeeds (nothing to do).
    bool Revoke(const std::filesystem::path& project_root, std::string& error);

    std::vector<std::filesystem::path> trusted() const;

    // The inventory for `project_root`, including the content provider's
    // contribution once SetContentProvider ran (main, after plugins load).
    ProjectLocalInventory Inspect(const std::filesystem::path& project_root) const {
        return InspectProjectLocal(project_root, content_provider_);
    }
    void SetContentProvider(const ProjectContentProvider* provider) { content_provider_ = provider; }

private:
    bool Update(const std::filesystem::path& project_root, bool trust, std::string& error);

    std::filesystem::path state_file_;
    ProjectLocalMode mode_;
    const ProjectContentProvider* content_provider_ = nullptr;
    mutable std::mutex mutex_;
    std::set<std::string> roots_;  // canonical git-root paths, guarded by mutex_
};

}  // namespace minimal::config
