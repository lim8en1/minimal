#pragma once

// The skills plugin's preset-resolution extensions (registered through
// minimal_plugin_register_preset_extensions): what each resolved preset gets.

#include "minimal/agent/preset_extensions.hpp"
#include "skill_access.hpp"
#include "skill_registry.hpp"

namespace minimal::skills {

// The preset's skills for its project root: its bound "skills" layer resolved
// against the loaded skills, clamped to the spawning parent's set when there
// is one.
SkillAccess ResolvedFor(const agent::PresetToolContext& ctx, const SkillRegistry& registry);

// The `skill` tool's per-preset definition: the base schema plus the preset's
// listing, or an EMPTY-named definition (tool withheld) when nothing is
// loadable.
agent::PresetToolDefinitionBuilder MakeSkillDefinitionBuilder(const SkillRegistry& registry);

// The preloaded skill bodies appended to the preset's system prompt.
agent::PresetPromptAppendix MakePreloadAppendix(const SkillRegistry& registry);

}  // namespace minimal::skills
