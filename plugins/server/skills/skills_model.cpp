#include "skills_model.hpp"

#include <string>

#include "skill_tool.hpp"

namespace minimal::skills {
namespace {

using tools::PermissionAnswer;
using tools::PermissionMode;
using tools::PermissionStage;

const char* ModeName(SkillMode mode) {
    switch (mode) {
        case SkillMode::Preload: return "preload";
        case SkillMode::Allow: return "allow";
        case SkillMode::Ask: return "ask";
        case SkillMode::Deny: return "deny";
    }
    return "deny";
}

class SkillsPermissionModel final : public tools::PermissionModel {
public:
    explicit SkillsPermissionModel(const SkillRegistry* registry) : registry_(registry) {}

    unsigned Replaces() const override { return static_cast<unsigned>(PermissionStage::kBase); }

    bool Claims(std::string_view permission_key) const override {
        return permission_key == kSkillToolName;
    }

    tools::PermissionLayer BuildLayer(const nlohmann::json& global_slice,
                                      const nlohmann::json& preset_slice,
                                      bool preset_trusted) const override {
        (void)preset_trusted;  // skills are prompt content: a preset's list stands as written
        auto layer = std::make_shared<SkillsLayer>();
        layer->access = ParseSkillAccess(global_slice, "permissions.json plugins.skills");
        for (const auto& [name, mode] :
             ParseSkillAccess(preset_slice, "plugin_permissions.skills")) {
            layer->access[name] = mode;
        }
        return layer;
    }

    PermissionAnswer Resolve(PermissionStage stage,
                             const tools::PermissionQuery& query) const override {
        (void)stage;
        const auto* layer = static_cast<const SkillsLayer*>(query.layer);
        if (layer == nullptr) return {PermissionMode::AlwaysDeny, "no skills are configured"};
        const auto it = query.input.find("name");
        if (it == query.input.end() || !it->is_string()) {
            // A malformed call: let execute answer it with its own error.
            return {PermissionMode::AlwaysAllow, "skill name missing"};
        }
        const std::string name = it->get<std::string>();
        const SkillMode mode = ModeFor(layer->access, name);
        const std::string why = "skills " + name + "=" + ModeName(mode);
        switch (mode) {
            case SkillMode::Allow: return {PermissionMode::AlwaysAllow, why};
            case SkillMode::Ask: return {PermissionMode::Ask, why};
            case SkillMode::Preload:
                return {PermissionMode::AlwaysDeny, why + " (already in your context)"};
            case SkillMode::Deny: break;
        }
        return {PermissionMode::AlwaysDeny, why};
    }

    // A name matching no global skill (a typo, or a skill that is not
    // installed): resolution drops it silently, so the startup check names it.
    // A known skill that failed to load is reported once, at plugin load.
    std::vector<std::string> LintSlices(const nlohmann::json& global_slice,
                                        const nlohmann::json& preset_slice) const override {
        std::vector<std::string> out;
        if (registry_ == nullptr) return out;
        const nlohmann::json& slice = preset_slice.is_null() ? global_slice : preset_slice;
        SkillAccess access;
        try {
            access = ParseSkillAccess(slice, "skills");
        } catch (const std::exception&) {
            return out;  // malformed: BuildLayer reports it as the preset's load error
        }
        for (const auto& [name, mode] : access) {
            (void)mode;
            if (name != "*" && registry_->Find(name) == nullptr) {
                out.push_back("skill '" + name + "' is unknown");
            }
        }
        return out;
    }

    std::string Describe(const void* layer) const override {
        const auto* skills = static_cast<const SkillsLayer*>(layer);
        if (skills == nullptr || skills->access.empty()) return "skills: none";
        std::string out = "skills:";
        for (const auto& [name, mode] : skills->access) {
            out += " " + name + "=" + ModeName(mode);
        }
        return out;
    }

private:
    const SkillRegistry* registry_;
};

}  // namespace

std::shared_ptr<const tools::PermissionModel> MakeSkillsPermissionModel(
    const SkillRegistry* registry) {
    return std::make_shared<SkillsPermissionModel>(registry);
}

const SkillsLayer* FindSkillsLayer(const tools::PermissionModels& models) {
    for (const auto& bound : models) {
        if (bound.model != nullptr && bound.model->Claims(kSkillToolName)) {
            return static_cast<const SkillsLayer*>(bound.layer.get());
        }
    }
    return nullptr;
}

}  // namespace minimal::skills
