#include "minimal/tools/skill_tool.hpp"

#include <algorithm>
#include <optional>
#include <utility>

namespace minimal::tools {
namespace {

ToolResult Error(std::string message) { return ToolResult{true, std::move(message)}; }

// The resolved access row for a requested skill, or nullptr when the session's
// preset does not offer it at all (denied, or never listed).
const ToolContext::SkillAccess* FindAccess(const std::vector<ToolContext::SkillAccess>& allowed,
                                           const std::string& name) {
    for (const auto& access : allowed) {
        if (access.name == name) return &access;
    }
    return nullptr;
}

// What the error message offers the model instead: allowed ∩ loaded, sorted
// (AllFor() is sorted), so it can recover by picking one of these.
std::string AvailableSkills(const config::SkillRegistry& registry,
                            const std::vector<ToolContext::SkillAccess>& allowed,
                            const std::filesystem::path& project_root) {
    std::string out;
    for (const auto& meta : registry.AllFor(project_root)) {
        if (!meta.loaded || FindAccess(allowed, meta.name) == nullptr) continue;
        if (!out.empty()) out += ", ";
        out += meta.name;
    }
    return out;
}

}  // namespace

SkillTool::SkillTool(const config::SkillRegistry& registry) : registry_(registry) {
    spec_.name = "skill";
    spec_.category = ToolCategory::ReadOnly;
    spec_.definition.name = spec_.name;
    spec_.definition.description =
        "Load the full instructions for a skill by name. The skills available in this session, "
        "with one-line descriptions, are listed in your system prompt.";
    spec_.definition.input_schema = {
        {"type", "object"},
        {"properties",
         {{"name", {{"type", "string"},
                    {"description", "Skill name, exactly as listed in the system prompt"}}}}},
        {"required", nlohmann::json::array({"name"})}};
}

ToolResult SkillTool::execute(const nlohmann::json& input, ToolContext& ctx) {
    if (!input.contains("name") || !input.at("name").is_string() ||
        input.at("name").get<std::string>().empty()) {
        return Error("missing required string field: name");
    }
    const std::string name = input.at("name").get<std::string>();

    // Unknown first, then load status BEFORE the allowed check: a skill that
    // failed to load is unusable by every preset, and its reason (a config
    // problem) is what the operator needs to see -- "not available in this
    // session" would misattribute the failure to the preset. The session's
    // project root scopes the lookup: a project's skills/ (and its
    // Claude-compatible overlays) shadow and extend the global library, the
    // same view the preset's system-prompt listing was built from.
    const config::SkillMeta* meta = registry_.FindFor(ctx.project_root, name);
    if (meta == nullptr) {
        return Error("unknown skill: " + name);
    }
    if (!meta->loaded) {
        return Error("skill '" + name + "' is not loaded: " + meta->load_error);
    }

    if (FindAccess(ctx.allowed_skills, name) == nullptr) {
        const std::string available = AvailableSkills(registry_, ctx.allowed_skills, ctx.project_root);
        return Error("skill '" + name + "' is not available in this session. Available skills: " +
                     (available.empty() ? "(none)" : std::move(available)));
    }

    // An ask-mode skill never reaches this point unprompted: mode_override_for
    // narrows the executor's resolution to Ask, and a satisfied grant (the
    // operator's "always") skips the prompt on every later load. Preloaded
    // skills are not in allowed_skills at all -- their bodies are already in
    // the prompt.
    const auto body = registry_.LoadBodyFor(ctx.project_root, name);
    if (!body) {
        return Error("failed to read skill body: " + name);
    }
    return ToolResult{false, std::move(*body)};
}

std::optional<PermissionMode> SkillTool::mode_override_for(const nlohmann::json& input,
                                                           const ToolContext& ctx) const {
    if (!input.is_object() || !input.contains("name") || !input.at("name").is_string()) {
        return std::nullopt;
    }
    const auto* access = FindAccess(ctx.allowed_skills, input.at("name").get<std::string>());
    if (access == nullptr || !access->ask) return std::nullopt;
    // Narrow only: the executor folds this with most_restrictive before the run
    // mode, so bypass still washes it to allow and allow-only clamps it to deny.
    return PermissionMode::Ask;
}

PermissionDisclosure SkillTool::Disclose(const nlohmann::json& input,
                                         const ToolContext& ctx) const {
    (void)ctx;
    PermissionDisclosure disclosure;
    disclosure.summary = "load skill";
    if (input.is_object() && input.contains("name") && input.at("name").is_string()) {
        const std::string name = input.at("name").get<std::string>();
        disclosure.summary = "load skill: " + name;
        // The stable, per-skill grant key: "always" on this offer silences every
        // later load of THIS skill for the session, and cannot cover another.
        disclosure.grants.push_back(GrantOffer{"skill:" + name, "skill " + name});
    }
    disclosure.complete = true;
    return disclosure;
}

}  // namespace minimal::tools
