#pragma once

// The skills plugin's permission model: it governs the `skill` tool, per skill
// name. Slices (opaque to core):
//   permissions.json   plugins.skills            {"<skill>|*": allow|ask|deny|preload}
//   preset frontmatter plugin_permissions.skills  (same shape, or a list = allow)
// The preset's entry for a name replaces the operator's. A preload entry is
// not callable (its body is in the system prompt), so it resolves to deny,
// like an absent one.

#include <memory>

#include "minimal/tools/permission_model.hpp"
#include "skill_access.hpp"

namespace minimal::skills {

// The layer BuildLayer produces and every consumer of a bound "skills" model
// reads back (the definition builder and the preload appendix included).
struct SkillsLayer {
    SkillAccess access;  // merged global + preset, "*" not yet expanded
};

std::shared_ptr<const tools::PermissionModel> MakeSkillsPermissionModel();

// The bound "skills" layer in a preset's models, or null.
const SkillsLayer* FindSkillsLayer(const tools::PermissionModels& models);

}  // namespace minimal::skills
