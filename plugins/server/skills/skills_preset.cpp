#include "skills_preset.hpp"

#include <string>

#include "skill_tool.hpp"
#include "skills_model.hpp"

namespace minimal::skills {

SkillAccess ResolvedFor(const agent::PresetToolContext& ctx, const SkillRegistry& registry) {
    const auto merged = registry.AllFor(ctx.project_root);
    const auto* layer = FindSkillsLayer(ctx.preset.permission_models);
    SkillAccess resolved = ResolveForRoot(layer != nullptr ? layer->access : SkillAccess{}, merged);
    if (ctx.parent != nullptr) {
        const auto* parent_layer = FindSkillsLayer(ctx.parent->permission_models);
        resolved = ClampToParent(
            resolved,
            ResolveForRoot(parent_layer != nullptr ? parent_layer->access : SkillAccess{}, merged));
    }
    return resolved;
}

agent::PresetToolDefinitionBuilder MakeSkillDefinitionBuilder(const SkillRegistry& registry) {
    return [&registry](const agent::PresetToolContext& ctx) {
        std::string listing =
            RenderListing(ResolvedFor(ctx, registry), registry.AllFor(ctx.project_root));
        llm::ToolDefinition def;
        if (listing.empty()) return def;  // nothing loadable: withhold the tool
        def = MakeSkillToolDefinition();
        def.description += "\n\n" + std::move(listing);
        return def;
    };
}

agent::PresetPromptAppendix MakePreloadAppendix(const SkillRegistry& registry) {
    return [&registry](const agent::PresetToolContext& ctx) {
        return RenderPreloads(ResolvedFor(ctx, registry), registry, ctx.project_root);
    };
}

}  // namespace minimal::skills
