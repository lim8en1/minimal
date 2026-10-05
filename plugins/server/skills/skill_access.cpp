#include "skill_access.hpp"

#include <algorithm>
#include <stdexcept>

#include "minimal/common/logger.hpp"

namespace minimal::skills {

SkillAccess ParseSkillAccess(const nlohmann::json& slice, const std::string& where) {
    SkillAccess out;
    if (slice.is_null()) return out;
    if (slice.is_array()) {
        for (const auto& item : slice) {
            if (!item.is_string() || item.get<std::string>().empty()) {
                throw std::runtime_error(where + ": skill names must be non-empty strings");
            }
            out[item.get<std::string>()] = SkillMode::Allow;
        }
        return out;
    }
    if (!slice.is_object()) {
        throw std::runtime_error(where + ": must be a list of skill names or a name->mode map");
    }
    for (const auto& [name, value] : slice.items()) {
        if (name.empty()) throw std::runtime_error(where + ": empty skill name");
        const std::string mode = value.is_string() ? value.get<std::string>() : std::string();
        if (mode == "allow") {
            out[name] = SkillMode::Allow;
        } else if (mode == "ask") {
            out[name] = SkillMode::Ask;
        } else if (mode == "deny") {
            out[name] = SkillMode::Deny;
        } else if (mode == "preload") {
            out[name] = SkillMode::Preload;
        } else {
            throw std::runtime_error(where + "." + name + ": must be allow, ask, deny or preload");
        }
    }
    return out;
}

SkillMode ModeFor(const SkillAccess& access, const std::string& name) {
    if (const auto it = access.find(name); it != access.end()) return it->second;
    if (const auto star = access.find("*"); star != access.end()) return star->second;
    return SkillMode::Deny;
}

SkillAccess ResolveForRoot(const SkillAccess& access, const std::vector<SkillMeta>& merged) {
    SkillAccess out;
    for (const auto& meta : merged) {
        if (!meta.loaded) continue;
        const SkillMode mode = ModeFor(access, meta.name);
        if (mode != SkillMode::Deny) out[meta.name] = mode;
    }
    return out;
}

SkillAccess ClampToParent(const SkillAccess& child, const SkillAccess& parent) {
    SkillAccess out;
    for (const auto& [name, mode] : child) {
        const auto it = parent.find(name);
        if (it == parent.end()) continue;
        const SkillMode clamped = std::max(it->second, mode);
        if (clamped != SkillMode::Deny) out[name] = clamped;
    }
    return out;
}

std::string RenderListing(const SkillAccess& resolved, const std::vector<SkillMeta>& merged) {
    std::string out;
    for (const auto& [name, mode] : resolved) {
        if (mode != SkillMode::Allow && mode != SkillMode::Ask) continue;
        const auto meta = std::find_if(merged.begin(), merged.end(),
                                       [&](const SkillMeta& m) { return m.name == name; });
        if (meta == merged.end()) continue;
        if (out.empty()) {
            out = "Available skills:\n";
        } else {
            out += "\n";
        }
        out += "- " + meta->name + ": " + meta->description +
               (mode == SkillMode::Ask ? " (asks before loading)" : "");
    }
    return out;
}

std::string RenderPreloads(const SkillAccess& resolved, const SkillRegistry& registry,
                           const std::filesystem::path& project_root) {
    std::string out;
    for (const auto& [name, mode] : resolved) {
        if (mode != SkillMode::Preload) continue;
        const auto body = registry.LoadBodyFor(project_root, name);
        if (!body) {
            common::DefaultLogger().warn(
                "skill '{}' is marked preload but its body could not be read; skipped", name);
            continue;
        }
        if (out.empty()) {
            out = "\n## Preloaded skills\n\n"
                  "The full instructions for these skills are ALREADY in your context; "
                  "do not call the `skill` tool for them.\n\n";
        }
        out += "### " + name + "\n\n" + *body + "\n";
    }
    return out;
}

}  // namespace minimal::skills
