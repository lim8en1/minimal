#include "minimal/tools/permission_model.hpp"

#include <stdexcept>
#include <utility>

#include "minimal/mcp/naming.hpp"
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

PermissionMode ParseMode(const nlohmann::json& value, const std::string& where) {
    if (value.is_string()) {
        if (auto mode = mode_from_name(value.get<std::string>())) return *mode;
    }
    throw std::runtime_error(where + " must be \"allow\", \"ask\" or \"deny\"");
}

class McpPermissionModel final : public PermissionModel {
public:
    unsigned Replaces() const override { return static_cast<unsigned>(PermissionStage::kBase); }

    bool Claims(std::string_view permission_key) const override {
        return !mcp::McpServerOf(permission_key).empty();
    }

    // global {"default": mode}; preset {"servers": {server: mode}}.
    PermissionLayer BuildLayer(const nlohmann::json& global_slice,
                               const nlohmann::json& preset_slice,
                               bool preset_trusted) const override {
        PermissionMode global_default = PermissionMode::Ask;
        if (global_slice.is_object() && global_slice.contains("default")) {
            global_default = ParseMode(global_slice.at("default"), "mcp default");
        }
        std::unordered_map<std::string, PermissionMode> per_server;
        if (preset_slice.is_object() && preset_slice.contains("servers")) {
            const auto& servers = preset_slice.at("servers");
            if (!servers.is_object()) {
                throw std::runtime_error("mcp servers must be a map of {server: mode}");
            }
            for (const auto& [server, mode] : servers.items()) {
                per_server[server] = ParseMode(mode, "mcp server '" + server + "'");
            }
        }
        return std::make_shared<const McpPermissionPolicy>(
            build_mcp_policy(per_server, global_default, preset_trusted));
    }

    PermissionAnswer Resolve(PermissionStage stage, const PermissionQuery& query) const override {
        (void)stage;  // kBase is the only stage this model replaces
        static const McpPermissionPolicy kDefault;
        const auto& policy = query.layer != nullptr
                                 ? *static_cast<const McpPermissionPolicy*>(query.layer)
                                 : kDefault;
        const std::string server = mcp::McpServerOf(query.permission_key);
        PermissionAnswer answer;
        if (query.core_policy != nullptr) {
            answer.mode = resolve_mcp_tool_mode(*query.core_policy, policy, server,
                                                query.permission_key, &answer.why);
        } else {
            answer.mode = policy.mode_for(server);
            answer.why = "mcp " + server + "=" + std::string(mode_name(answer.mode));
        }
        return answer;
    }

    std::string_view HookCategory() const override { return "mcp"; }

    std::string Describe(const void* layer) const override {
        if (layer == nullptr) return {};
        const auto& policy = *static_cast<const McpPermissionPolicy*>(layer);
        std::string out = "default=" + std::string(mode_name(policy.default_mode));
        for (const auto& [server, mode] : policy.per_server) {
            out += " " + server + "=" + std::string(mode_name(mode));
        }
        return out;
    }
};

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

std::shared_ptr<const PermissionModel> MakeMcpPermissionModel() {
    static const auto model = std::make_shared<const McpPermissionModel>();
    return model;
}

BoundPermissionModel BindMcpPermissionModel(McpPermissionPolicy policy) {
    return BoundPermissionModel{std::string(kMcpPermissionModelName), MakeMcpPermissionModel(),
                                std::make_shared<const McpPermissionPolicy>(std::move(policy))};
}

const McpPermissionPolicy* FindMcpPolicy(const PermissionModels& models) {
    for (const auto& bound : models) {
        if (bound.name == kMcpPermissionModelName && bound.layer) {
            return static_cast<const McpPermissionPolicy*>(bound.layer.get());
        }
    }
    return nullptr;
}

}  // namespace minimal::tools
