#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace minimal::config {

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
// Thread-safety: const after Load(); LoadBody() holds no mutable state, so
// concurrent calls from multiple session workers are safe.
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

private:
    std::vector<SkillMeta> skills_;
    std::string scan_error_;
};

}  // namespace minimal::config
