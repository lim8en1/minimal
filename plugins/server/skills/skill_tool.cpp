#include "skill_tool.hpp"

#include <algorithm>
#include <optional>
#include <system_error>
#include <utility>

namespace minimal::skills {

using tools::PermissionDisclosure;
using tools::ToolContext;
using tools::ToolResult;

namespace {

ToolResult Error(std::string message) { return ToolResult{true, std::move(message)}; }

}  // namespace

llm::ToolDefinition MakeSkillToolDefinition() {
    llm::ToolDefinition def;
    def.name = kSkillToolName;
    def.description =
        "Load the full instructions for a skill by name. The result includes the "
        "skill's directory, which may contain additional supporting files referenced "
        "by the skill.";
    def.input_schema = {
        {"type", "object"},
        {"properties",
         {{"name", {{"type", "string"},
                    {"description", "Skill name, exactly as listed in this tool's description"}}}}},
        {"required", nlohmann::json::array({"name"})}};
    return def;
}

SkillTool::SkillTool(const SkillRegistry& registry) : registry_(registry) {
    spec_.name = kSkillToolName;
    spec_.category = tools::ToolCategory::ReadOnly;
    spec_.definition = MakeSkillToolDefinition();
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
    const SkillMeta* meta = registry_.FindFor(ctx.project_root, name);
    if (meta == nullptr) {
        return Error("unknown skill: " + name);
    }
    if (!meta->loaded) {
        return Error("skill '" + name + "' is not loaded: " + meta->load_error);
    }

    // Access was settled at the gate (the skills permission model): a call
    // reaching here is allowed, or an ask the operator answered.
    const auto body = registry_.LoadBodyFor(ctx.project_root, name);
    if (!body) {
        return Error("failed to read skill body: " + name);
    }
    // Surface the skill's directory so the body can point at sibling files
    // ("in that case load additional.md") and the model can resolve them with
    // the read tool. Most skills are single-file; the line is unconditional so
    // the model never has to guess whether the directory exists. Canonicalized
    // so global and project-overlay skills report the same form (the global
    // scan keeps the config dir as given; the overlay canonicalizes its root),
    // and the read tool's confinement canonicalizes too.
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::weakly_canonical(meta->body_path.parent_path(), ec);
    const std::string header = "skill directory: " + (ec ? meta->body_path.parent_path() : dir).string() + "\n\n";
    return ToolResult{false, header + *body, nlohmann::json{{"kind", "markdown"}}};
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
        disclosure.grants.push_back(tools::GrantOffer{"skill:" + name, "skill " + name});
    }
    disclosure.complete = true;
    return disclosure;
}

}  // namespace minimal::skills
