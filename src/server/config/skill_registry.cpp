#include "minimal/config/skill_registry.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

#include "minimal/config/frontmatter.hpp"

namespace minimal::config {
namespace {

std::string ReadFileOrEmpty(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) return {};
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

// Evaluates one candidate under skills/. Never throws: every failure mode
// becomes an entry with loaded=false and the reason a human (or the model, via
// SkillTool) can act on.
SkillMeta LoadCandidate(const std::filesystem::path& candidate) {
    SkillMeta meta;
    meta.name = candidate.filename().string();

    if (!std::filesystem::is_directory(candidate)) {
        meta.load_error = "expected a skill directory containing SKILL.md, found a file";
        return meta;
    }

    const std::filesystem::path skill_md = candidate / "SKILL.md";
    meta.body_path = skill_md;
    if (!std::filesystem::is_regular_file(skill_md)) {
        meta.load_error = "no SKILL.md in directory";
        return meta;
    }

    ParsedFrontmatter parsed;
    try {
        parsed = parse_frontmatter_markdown(ReadFileOrEmpty(skill_md));
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
    // the listing, and the file would each name the skill differently.
    const YAML::Node declared = node["name"];
    if (declared && declared.IsScalar() && declared.as<std::string>() != meta.name) {
        meta.load_error = "frontmatter name '" + declared.as<std::string>() + "' does not match '" +
                          meta.name + "'";
        return meta;
    }

    const YAML::Node description = node["description"];
    if (!description || !description.IsScalar() || description.as<std::string>().empty()) {
        meta.load_error = "missing required frontmatter field: description";
        return meta;
    }

    meta.description = description.as<std::string>();
    meta.loaded = true;
    return meta;
}

}  // namespace

SkillRegistry SkillRegistry::Load(std::filesystem::path config_dir) {
    SkillRegistry registry;
    const std::filesystem::path skills_dir = config_dir / "skills";

    std::error_code ec;
    if (!std::filesystem::is_directory(skills_dir, ec)) return registry;

    for (const auto& entry : std::filesystem::directory_iterator(skills_dir, ec)) {
        registry.skills_.push_back(LoadCandidate(entry.path()));
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

    ParsedFrontmatter parsed;
    try {
        parsed = parse_frontmatter_markdown(ReadFileOrEmpty(meta->body_path));
    } catch (const std::exception&) {
        return std::nullopt;
    }
    return parsed.body;
}

}  // namespace minimal::config
