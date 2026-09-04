#include "minimal/config/skill_registry.hpp"

#include <algorithm>
#include <fstream>
#include <mutex>
#include <sstream>
#include <system_error>
#include <utility>

#include "minimal/common/logger.hpp"
#include "minimal/config/frontmatter.hpp"
#include "minimal/config/project_dirs.hpp"

namespace minimal::config {
namespace {

// nullopt distinguishes "could not read" from "read an empty file". Collapsing
// the two made an unreadable SKILL.md report "missing frontmatter block", which
// sends the operator (and, via SkillTool, the model) to fix frontmatter that was
// never the problem.
std::optional<std::string> ReadFile(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) return std::nullopt;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    if (in.bad()) return std::nullopt;
    return buffer.str();
}

// A description goes into the system prompt as one bullet per skill. Embedded
// newlines -- easy to produce with `description: |` -- would break that
// structure inside the cached prefix, so they are folded here rather than
// rejected: the content is fine, only its shape is wrong for the listing.
std::string SingleLine(std::string text) {
    for (char& c : text) {
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    }
    std::string out;
    out.reserve(text.size());
    bool pending_space = false;
    for (const char c : text) {
        if (c == ' ') {
            pending_space = !out.empty();
            continue;
        }
        if (pending_space) out += ' ';
        pending_space = false;
        out += c;
    }
    return out;
}

// Evaluates one candidate under skills/. Never throws: every failure mode
// becomes an entry with loaded=false and the reason a human (or the model, via
// SkillTool) can act on.
SkillMeta LoadCandidate(const std::filesystem::path& candidate) {
    SkillMeta meta;
    meta.name = candidate.filename().string();

    // The error_code overloads, not the throwing ones: this function is
    // documented never to throw, and the throwing overloads turn an unreadable
    // parent directory into a filesystem_error straight out of the loader.
    // The Load() below already had the right shape.
    std::error_code ec;
    if (!std::filesystem::is_directory(candidate, ec) || ec) {
        meta.load_error = "expected a skill directory containing SKILL.md, found a file";
        return meta;
    }

    const std::filesystem::path skill_md = candidate / "SKILL.md";
    meta.body_path = skill_md;
    ec.clear();
    if (!std::filesystem::is_regular_file(skill_md, ec) || ec) {
        meta.load_error = "no SKILL.md in directory";
        return meta;
    }

    const std::optional<std::string> contents = ReadFile(skill_md);
    if (!contents) {
        meta.load_error = "SKILL.md could not be read (check permissions)";
        return meta;
    }

    ParsedFrontmatter parsed;
    try {
        parsed = parse_frontmatter_markdown(*contents);
    } catch (const std::exception& e) {
        meta.load_error = std::string("frontmatter parse error: ") + e.what();
        return meta;
    }

    const YAML::Node& node = parsed.frontmatter;
    if (!node || !node.IsMap()) {
        meta.load_error = "missing frontmatter block";
        return meta;
    }

    // The directory name is the identity. A frontmatter `name` is optional but,
    // when present, must agree -- a disagreement means allowed_skills entries,
    // the listing, and the file would each name the skill differently. A
    // non-scalar `name` is an error rather than a silent skip: skipping would
    // bypass the very check that keeps those three in agreement.
    const YAML::Node declared = node["name"];
    if (declared) {
        if (!declared.IsScalar()) {
            meta.load_error = "frontmatter field 'name' must be a string";
            return meta;
        }
        if (declared.as<std::string>() != meta.name) {
            meta.load_error = "frontmatter name '" + declared.as<std::string>() +
                              "' does not match '" + meta.name + "'";
            return meta;
        }
    }

    const YAML::Node description = node["description"];
    if (!description || !description.IsScalar()) {
        meta.load_error = "missing required frontmatter field: description";
        return meta;
    }

    meta.description = SingleLine(description.as<std::string>());
    if (meta.description.empty()) {
        meta.load_error = "missing required frontmatter field: description";
        return meta;
    }
    meta.loaded = true;
    return meta;
}

// Iterates one skills/ directory, appending a candidate per direct entry. Never
// throws; a directory that cannot be read reports false with the reason -- the
// caller decides whether that is a startup scan_error_ (global) or a logged
// warning (project overlay).
bool ScanSkillsDir(const std::filesystem::path& skills_dir, std::vector<SkillMeta>& out,
                   std::string& error) {
    std::error_code ec;
    std::filesystem::directory_iterator it(skills_dir, ec);
    if (ec) {
        error = "cannot read " + skills_dir.string() + ": " + ec.message();
        return false;
    }

    for (; it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (ec) {
            error = "stopped scanning " + skills_dir.string() + ": " + ec.message();
            return false;
        }
        // Dotfiles are never skills. Without this every macOS config directory
        // that has been opened in Finder reports .DS_Store as a broken skill,
        // and a startup check that always complains is one nobody reads. A
        // non-dot stray (skills/foo.md) still gets its error: that is a real
        // mistake worth naming.
        const std::string name = it->path().filename().string();
        if (!name.empty() && name.front() == '.') continue;

        out.push_back(LoadCandidate(it->path()));
    }
    return true;
}

}  // namespace

SkillRegistry SkillRegistry::Load(std::filesystem::path config_dir) {
    SkillRegistry registry;
    const std::filesystem::path skills_dir = config_dir / "skills";

    // An absent skills/ is normal and silent. One that exists but cannot be
    // stat'd or read is not: without this distinction a permissions problem
    // produces an empty registry indistinguishable from "no skills configured",
    // and the operator gets no signal at all.
    // is_directory() reports ENOENT through `ec` rather than a clean false, so
    // "absent" must be filtered out explicitly -- otherwise the common, correct
    // case of having no skills at all raises a warning, which is the same false
    // alarm the dotfile skip exists to prevent.
    std::error_code ec;
    const bool is_dir = std::filesystem::is_directory(skills_dir, ec);
    if (ec == std::errc::no_such_file_or_directory || ec == std::errc::not_a_directory) {
        return registry;
    }
    if (ec) {
        registry.scan_error_ = "cannot stat " + skills_dir.string() + ": " + ec.message();
        return registry;
    }
    if (!is_dir) return registry;

    std::string error;
    if (!ScanSkillsDir(skills_dir, registry.skills_, error)) {
        registry.scan_error_ = error;
    }

    std::sort(registry.skills_.begin(), registry.skills_.end(),
              [](const SkillMeta& a, const SkillMeta& b) { return a.name < b.name; });
    return registry;
}

const SkillMeta* SkillRegistry::Find(std::string_view name) const {
    for (const auto& meta : skills_) {
        if (meta.name == name) return &meta;
    }
    return nullptr;
}

std::optional<std::string> SkillRegistry::LoadBody(std::string_view name) const {
    const SkillMeta* meta = Find(name);
    if (meta == nullptr || !meta->loaded) return std::nullopt;

    // Read failure must not fall through as an empty body: that would hand the
    // model a successful-looking skill with no instructions in it.
    const std::optional<std::string> contents = ReadFile(meta->body_path);
    if (!contents) return std::nullopt;

    ParsedFrontmatter parsed;
    try {
        parsed = parse_frontmatter_markdown(*contents);
    } catch (const std::exception&) {
        return std::nullopt;
    }
    return parsed.body;
}

// --- project-local overlay -----------------------------------------------------

namespace {

// Subdirectory names scanned for skills within one project directory, in
// preference order. "skills" is minimal's own convention; the dot-prefixed
// paths are the Claude- and agent-compatible locations, so a project keeps one
// skill library for every harness that reads it. (ScanSkillsDir's dotfile rule
// applies to ENTRIES of a scanned directory, never to these explicit names.)
constexpr const char* kProjectSkillSubdirs[] = {"skills", ".claude/skills", ".agents/skills"};

std::filesystem::path CanonicalPath(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
    if (ec) canonical = path;
    return canonical;
}

// Every project skill the walk finds, nearest directory winning: the first
// occurrence of a name in walk order survives, and a shadowed duplicate is
// logged. This builder runs at most once per canonical root (the caller
// caches), so the warnings are once per root, not once per call.
std::vector<SkillMeta> BuildOverlay(const ProjectDirs& dirs) {
    std::vector<SkillMeta> overlay;
    std::unordered_map<std::string, std::size_t> index;
    for (const auto& dir : dirs.dirs()) {
        for (const char* sub : kProjectSkillSubdirs) {
            const std::filesystem::path skills_dir = dir / sub;
            std::error_code ec;
            if (!std::filesystem::is_directory(skills_dir, ec) || ec) continue;

            std::vector<SkillMeta> found;
            std::string error;
            if (!ScanSkillsDir(skills_dir, found, error)) {
                // A project directory that cannot be read is a warning, not a
                // fatal scan_error_: the session must still start, and the
                // operator gets the reason either way.
                common::DefaultLogger().warn("project skills scan failed: {}", error);
                continue;
            }
            for (auto& meta : found) {
                auto [it, inserted] = index.try_emplace(meta.name, overlay.size());
                if (inserted) {
                    overlay.push_back(std::move(meta));
                } else {
                    common::DefaultLogger().warn(
                        "project skill '{}' from {} is shadowed by a nearer directory",
                        meta.name, meta.body_path.string());
                }
            }
        }
    }
    std::sort(overlay.begin(), overlay.end(),
              [](const SkillMeta& a, const SkillMeta& b) { return a.name < b.name; });
    return overlay;
}

}  // namespace

const std::vector<SkillMeta>& SkillRegistry::MergedFor(
    const std::filesystem::path& project_root) const {
    std::lock_guard<std::mutex> lock(cache_->mu);
    auto [it, inserted] = cache_->merged.try_emplace(CanonicalPath(project_root));
    if (inserted) {
        std::vector<SkillMeta> merged = skills_;
        for (auto& meta : BuildOverlay(ProjectDirs(project_root))) {
            auto found = std::find_if(merged.begin(), merged.end(),
                                      [&meta](const SkillMeta& global) {
                                          return global.name == meta.name;
                                      });
            if (found != merged.end()) {
                *found = std::move(meta);  // project shadows global
            } else {
                merged.push_back(std::move(meta));
            }
        }
        std::sort(merged.begin(), merged.end(),
                  [](const SkillMeta& a, const SkillMeta& b) { return a.name < b.name; });
        it->second = std::move(merged);
    }
    return it->second;
}

std::vector<SkillMeta> SkillRegistry::AllFor(const std::filesystem::path& project_root) const {
    if (project_root.empty()) return skills_;
    return MergedFor(project_root);
}

const SkillMeta* SkillRegistry::FindFor(const std::filesystem::path& project_root,
                                        std::string_view name) const {
    if (project_root.empty()) return Find(name);
    const std::vector<SkillMeta>& merged = MergedFor(project_root);
    for (const auto& meta : merged) {
        if (meta.name == name) return &meta;
    }
    return nullptr;
}

std::optional<std::string> SkillRegistry::LoadBodyFor(const std::filesystem::path& project_root,
                                                      std::string_view name) const {
    const SkillMeta* meta = FindFor(project_root, name);
    if (meta == nullptr || !meta->loaded) return std::nullopt;

    // Read failure must not fall through as an empty body: that would hand the
    // model a successful-looking skill with no instructions in it.
    const std::optional<std::string> contents = ReadFile(meta->body_path);
    if (!contents) return std::nullopt;

    ParsedFrontmatter parsed;
    try {
        parsed = parse_frontmatter_markdown(*contents);
    } catch (const std::exception&) {
        return std::nullopt;
    }
    return parsed.body;
}

}  // namespace minimal::config
