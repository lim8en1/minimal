#include "minimal/tools/skill_tool.hpp"

#include <algorithm>
#include <utility>

namespace minimal::tools {
namespace {

ToolResult Error(std::string message) { return ToolResult{true, std::move(message)}; }

bool SkillAllowed(const std::vector<std::string>& allowed_skills, const std::string& name) {
    return std::find(allowed_skills.begin(), allowed_skills.end(), "*") != allowed_skills.end() ||
           std::find(allowed_skills.begin(), allowed_skills.end(), name) != allowed_skills.end();
}

// What the error message offers the model instead: allowed ∩ loaded, sorted
// (All() is sorted), so it can recover by picking one of these.
std::string AvailableSkills(const config::SkillRegistry& registry,
                            const std::vector<std::string>& allowed_skills) {
    std::string out;
    for (const auto& meta : registry.All()) {
        if (!meta.loaded || !SkillAllowed(allowed_skills, meta.name)) continue;
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
    // session" would misattribute the failure to the preset.
    const config::SkillMeta* meta = registry_.Find(name);
    if (meta == nullptr) {
        return Error("unknown skill: " + name);
    }
    if (!meta->loaded) {
        return Error("skill '" + name + "' is not loaded: " + meta->load_error);
    }

    if (!SkillAllowed(ctx.allowed_skills, name)) {
        const std::string available = AvailableSkills(registry_, ctx.allowed_skills);
        return Error("skill '" + name + "' is not available in this session. Available skills: " +
                     (available.empty() ? "(none)" : std::move(available)));
    }

    const auto body = registry_.LoadBody(name);
    if (!body) {
        return Error("failed to read skill body: " + name);
    }
    return ToolResult{false, std::move(*body)};
}

}  // namespace minimal::tools
