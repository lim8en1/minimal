#pragma once

#include "minimal/config/skill_registry.hpp"
#include "minimal/tools/tool.hpp"

namespace minimal::tools {

// Core tool, not a plugin: a thin accessor over the core skill registry. A
// skill's resident cost is its frontmatter (rendered into the system prompt by
// the preset); this tool is the lazy-load trigger that returns the SKILL.md
// body on invocation.
//
// Category ReadOnly, so the permission model never prompts by default -- the
// per-skill gate is allowed_skills, enforced here at invocation time against
// ToolContext::allowed_skills (the session's active preset). The exception is
// an ASK-mode skill: mode_override_for narrows the executor's resolution to
// Ask for exactly that name, and Disclose offers the stable per-skill grant
// key ("skill:<name>") so the operator's "always" covers later loads of the
// SAME skill and no other.
class SkillTool : public Tool {
public:
    explicit SkillTool(const config::SkillRegistry& registry);

    const ToolSpec& spec() const override { return spec_; }
    ToolResult execute(const nlohmann::json& input, ToolContext& ctx) override;
    std::optional<PermissionMode> mode_override_for(const nlohmann::json& input,
                                                    const ToolContext& ctx) const override;
    PermissionDisclosure Disclose(const nlohmann::json& input,
                                  const ToolContext& ctx) const override;

private:
    const config::SkillRegistry& registry_;
    ToolSpec spec_;
};

}  // namespace minimal::tools
