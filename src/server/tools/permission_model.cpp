#include "minimal/tools/permission_model.hpp"

#include <stdexcept>
#include <utility>

#include "minimal/tools/tool_registry.hpp"

namespace minimal::tools {
namespace {

std::string ModelLabel(const std::string& owner) {
    return owner.empty() ? "core" : "plugin '" + owner + "'";
}

// A slice member, or null when the slices object does not carry one.
nlohmann::json SliceFor(const nlohmann::json& slices, const std::string& name) {
    if (!slices.is_object()) return nullptr;
    const auto it = slices.find(name);
    return it == slices.end() ? nlohmann::json() : *it;
}

}  // namespace

const BoundPermissionModel* FindClaimant(const PermissionModels& models,
                                         std::string_view permission_key) {
    for (const auto& bound : models) {
        if (bound.model && bound.model->Claims(permission_key)) return &bound;
    }
    return nullptr;
}

bool PermissionModelRegistry::Register(std::shared_ptr<const PermissionModel> model) {
    // One model per plugin, keyed by the plugin itself: a model registered
    // outside a plugin's registration window has no owner to key on.
    if (!model || attribution().empty()) return false;
    return AttributedRegistry::Register(attribution(), std::move(model));
}

PermissionModels PermissionModelRegistry::Bind(const nlohmann::json& global_slices,
                                               const nlohmann::json& preset_slices,
                                               bool preset_trusted) const {
    PermissionModels out;
    for (const auto& name : kinds()) {
        const auto& model = *FindEntry(name);
        try {
            out.push_back(BoundPermissionModel{
                name, model,
                model->BuildLayer(SliceFor(global_slices, name), SliceFor(preset_slices, name),
                                  preset_trusted)});
        } catch (const std::exception& e) {
            throw std::runtime_error("permission model '" + name + "': " + e.what());
        }
    }
    return out;
}

std::vector<std::string> PermissionModelRegistry::ValidateClaims(const ToolRegistry& tools) const {
    std::vector<std::string> errors;
    const auto names = kinds();
    for (const auto& attr : tools.attributions()) {
        std::vector<std::string> claimants;
        for (const auto& name : names) {
            if ((*FindEntry(name))->Claims(attr.tool_name)) claimants.push_back(name);
        }
        if (claimants.size() > 1) {
            std::string joined;
            for (const auto& name : claimants) joined += (joined.empty() ? "" : ", ") + name;
            errors.push_back("tool '" + attr.tool_name + "' is claimed by several permission models (" +
                             joined + ") -- disable one of the plugins");
            continue;
        }
        if (claimants.empty()) continue;
        const std::string& owner = *OwnerOf(claimants.front());
        if (owner != attr.plugin_name) {
            errors.push_back("permission model '" + claimants.front() + "' (" + ModelLabel(owner) +
                             ") claims tool '" + attr.tool_name + "' registered by " +
                             ModelLabel(attr.plugin_name) +
                             " -- a model may only govern its own plugin's tools");
        }
    }
    return errors;
}

}  // namespace minimal::tools
