#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace minimal::config {
class ProjectTrustQuery;
}  // namespace minimal::config

namespace minimal::skills {

// One candidate under skills/. EVERY entry directly under the directory gets a
// record -- including ones that failed to load. `loaded` plus `load_error` are
// the diagnostics; consumers (the system-prompt listing, SkillTool) filter on
// `loaded` rather than assuming All() is all healthy.
struct SkillMeta {
    std::string name;  // directory name under skills/ -- the canonical identity
    std::string description;  // frontmatter; empty when not loaded
    std::filesystem::path body_path;  // the SKILL.md candidate path
    bool loaded = false;
    std::string load_error;  // reason when !loaded
};

// The resident cost of a skill library is its frontmatter, nothing more: Load()
// reads only frontmatter, and bodies are read lazily by LoadBody(), on
// invocation. That is what makes a large skills/ directory cheap to have.
//
// Project-local overlay: alongside the global config_dir scan, the *For()
// methods merge an overlay scanned from a session's project directories
// (ProjectDirs, nearest git worktree root included) -- <dir>/skills/,
// <dir>/.claude/skills/, <dir>/.agents/skills/ per directory, with the
// Claude-compatible paths so a project can carry one skill library for every
// agent harness that reads it. The nearest directory wins on a name collision,
// and a project skill shadows a global one of the same name. A project overlay
// can only be discovered per session -- project_root is a SESSION property --
// so the merged view is built lazily per canonical root and cached; the frontmatter-only
// scan keeps that cost at resident cost, and project-local problems (a broken
// skill, an unreadable directory) surface through the same loaded/load_error
// entries and logs a global scan produces, at resolve time rather than startup.
//
// Thread-safety: const after Load(); the global accessors hold no mutable
// state, and the overlay cache is mutex-guarded, so concurrent calls from
// multiple session workers are safe. Pointers handed out by FindFor() stay
// valid for the process lifetime (the cache is never erased, and node-based
// containers keep element addresses stable).
class SkillRegistry {
public:
    // Scans config_dir/skills/. Never throws for individual candidate problems:
    // each becomes an entry with loaded=false and a reason. A missing skills/
    // directory is normal (skills are off unless a preset opts in) and yields
    // an empty registry; one that exists but cannot be read yields an empty
    // registry AND a scan_error(). Dotfiles are skipped -- .DS_Store is not a
    // broken skill. Entries come back sorted by name, so any consumer that
    // iterates All() is deterministic without re-sorting.
    static SkillRegistry Load(std::filesystem::path config_dir);

    // The project-local overlay is only consulted for roots `trust` allows; the
    // others see the global view, exactly as an empty project_root would. Null
    // (the default) applies no gate. Startup only: call before the registry is
    // shared; `trust` must outlive it.
    void SetProjectTrust(const config::ProjectTrustQuery* trust) { trust_ = trust; }

    const std::vector<SkillMeta>& All() const { return skills_; }

    // Non-empty when skills/ exists but could not be scanned. Distinct from an
    // empty registry, which is the normal "no skills configured" state -- a
    // permissions problem must not look like a deliberate absence.
    const std::string& scan_error() const { return scan_error_; }

    // null when no candidate exists under that name at all; non-null (possibly
    // !loaded) otherwise. Callers that need the failure reason use this rather
    // than LoadBody(), which reports only nullopt.
    const SkillMeta* Find(std::string_view name) const;

    // The lazy read: parses and returns the SKILL.md body, frontmatter stripped.
    // nullopt when the skill is unknown, not loaded, or unreadable at call time.
    std::optional<std::string> LoadBody(std::string_view name) const;

    // --- project-local overlay -------------------------------------------------
    // An empty project_root means "no project scope" and degenerates to the
    // global view. AllFor() returns a snapshot (global ∪ overlay, project
    // shadows global, sorted by name); FindFor() returns a pointer into the
    // cached merged view (see the thread-safety note above).

    // Sorted by name; deterministic, so a system-prompt listing built from it
    // is stable inside the cached prefix.
    std::vector<SkillMeta> AllFor(const std::filesystem::path& project_root) const;

    // Overlay first, then global: the nearest declaration wins.
    const SkillMeta* FindFor(const std::filesystem::path& project_root,
                             std::string_view name) const;

    std::optional<std::string> LoadBodyFor(const std::filesystem::path& project_root,
                                           std::string_view name) const;

private:
    // The merged view for one canonical project root, built on first use and
    // cached. GUARDED by cache->mu; callers get the reference only under it.
    // Heap-held through Load()'s by-value return: a mutex is neither movable
    // nor copyable, so a direct member would delete the move Load() needs.
    struct OverlayCache {
        std::mutex mu;
        // Keyed on the canonical root's STRING, not the path: hashing a
        // std::filesystem::path relies on the std::hash<path> specialization
        // added by LWG 3657, which not every standard library shipped when
        // marked; a string key needs nothing beyond the base standard.
        std::unordered_map<std::string, std::vector<SkillMeta>> merged;
    };
    const std::vector<SkillMeta>& MergedFor(const std::filesystem::path& project_root) const;

    // True when `project_root` has a project scope AND it may be loaded.
    bool ProjectScoped(const std::filesystem::path& project_root) const;

    const config::ProjectTrustQuery* trust_ = nullptr;
    std::vector<SkillMeta> skills_;
    std::string scan_error_;
    mutable std::unique_ptr<OverlayCache> cache_ = std::make_unique<OverlayCache>();
};

}  // namespace minimal::skills
