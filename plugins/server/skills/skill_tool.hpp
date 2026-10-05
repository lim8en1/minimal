#pragma once

#include "minimal/tools/tool.hpp"
#include "skill_registry.hpp"

namespace minimal::skills {

inline constexpr const char* kSkillToolName = "skill";

// The tool's static base definition, shared by the SkillTool constructor and
// the per-preset definition builder (plugin.cpp), which appends the preset's
// own skill listing -- so the schema lives in exactly one place.
llm::ToolDefinition MakeSkillToolDefinition();

// The lazy-load trigger: returns a skill's SKILL.md body on invocation. A
// skill's resident cost is its frontmatter, rendered into the tool's
// per-preset description.
//
// The per-skill gate is NOT here: the skills permission model (skills_model)
// governs this tool, so allow/ask/deny/preload, the subagent ceiling and the
// operator's kill switches are all resolved by core's gate before execute
// runs. Disclose offers the stable per-skill grant key ("skill:<name>") so an
// operator's "always" covers later loads of the SAME skill and no other.
class SkillTool : public tools::Tool {
public:
    explicit SkillTool(const SkillRegistry& registry);

    const tools::ToolSpec& spec() const override { return spec_; }
    tools::ToolResult execute(const nlohmann::json& input, tools::ToolContext& ctx) override;
    tools::PermissionDisclosure Disclose(const nlohmann::json& input,
                                         const tools::ToolContext& ctx) const override;

private:
    const SkillRegistry& registry_;
    tools::ToolSpec spec_;
};

}  // namespace minimal::skills
