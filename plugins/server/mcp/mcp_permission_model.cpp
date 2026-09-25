#include "minimal/mcp/mcp_permission_model.hpp"

#include <stdexcept>
#include <utility>

#include "minimal/mcp/naming.hpp"

namespace minimal::mcp {

using tools::PermissionMode;
using tools::rule_text;

namespace {

std::optional<PermissionMode> Fold(std::optional<PermissionMode> a,
                                   std::optional<PermissionMode> b) {
    if (a && b) return tools::most_restrictive(*a, *b);
    return a ? a : b;
}

PermissionMode ParseMode(const nlohmann::json& value, const std::string& where) {
    if (value.is_string()) {
        if (auto mode = tools::mode_from_name(value.get<std::string>())) return *mode;
    }
    throw std::runtime_error(where + " must be \"allow\", \"ask\" or \"deny\"");
}

class McpPermissionModel final : public tools::PermissionModel {
public:
    unsigned Replaces() const override {
        return static_cast<unsigned>(tools::PermissionStage::kBase);
    }

    bool Claims(std::string_view permission_key) const override {
        return !McpServerOf(permission_key).empty();
    }

    tools::PermissionLayer BuildLayer(const nlohmann::json& global_slice,
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
            BuildMcpPolicy(per_server, global_default, preset_trusted));
    }

    tools::PermissionAnswer Resolve(tools::PermissionStage stage,
                                    const tools::PermissionQuery& query) const override {
        (void)stage;  // kBase is the only stage this model replaces
        static const McpPermissionPolicy kDefault;
        const auto& policy = query.layer != nullptr
                                 ? *static_cast<const McpPermissionPolicy*>(query.layer)
                                 : kDefault;
        const std::string server = McpServerOf(query.permission_key);
        tools::PermissionAnswer answer;
        if (query.core_policy != nullptr) {
            answer.mode = ResolveMcpToolMode(*query.core_policy, policy, server,
                                             query.permission_key, &answer.why);
        } else {
            answer.mode = policy.mode_for(server);
            answer.why = "mcp " + server + "=" + std::string(tools::mode_name(answer.mode));
        }
        return answer;
    }

    std::string_view HookCategory() const override { return "mcp"; }

    std::string Describe(const void* layer) const override {
        if (layer == nullptr) return {};
        const auto& policy = *static_cast<const McpPermissionPolicy*>(layer);
        std::string out = "default=" + std::string(tools::mode_name(policy.default_mode));
        for (const auto& [server, mode] : policy.per_server) {
            out += " " + server + "=" + std::string(tools::mode_name(mode));
        }
        return out;
    }
};

}  // namespace

PermissionMode McpPermissionPolicy::mode_for(std::string_view server_name) const {
    if (auto it = per_server.find(std::string(server_name)); it != per_server.end()) {
        return it->second;
    }
    return default_mode;
}

McpPermissionPolicy BuildMcpPolicy(
    const std::unordered_map<std::string, PermissionMode>& preset_per_server,
    PermissionMode global_default_mode, bool preset_trusted) {
    McpPermissionPolicy policy;
    for (const auto& [server, mode] : preset_per_server) {
        policy.per_server[server] =
            preset_trusted ? mode : tools::most_restrictive(mode, global_default_mode);
    }
    policy.default_mode = global_default_mode;
    return policy;
}

PermissionMode ResolveMcpToolMode(const tools::PermissionPolicy& policy,
                                  const McpPermissionPolicy& mcp_policy,
                                  std::string_view server_name, std::string_view tool_name,
                                  std::string* reason) {
    const PermissionMode server_mode = mcp_policy.mode_for(server_name);
    // McpPermissionPolicy::per_server carries only the PRESET's entries (the
    // global side is default_mode), so its presence is "the preset spoke".
    const bool preset_spoke_for_server =
        mcp_policy.per_server.count(std::string(server_name)) != 0;
    const std::string server_text =
        rule_text(preset_spoke_for_server ? "preset mcp server" : "mcp default", server_name,
                  server_mode);
    auto answer = [&](PermissionMode mode, std::string why) {
        if (reason != nullptr) *reason = std::move(why);
        return mode;
    };

    const auto rules = policy.per_tool_rules(tool_name);
    if (!rules.layered) {
        if (rules.flat) {
            return answer(*rules.flat, rule_text("per-tool rule", tool_name, *rules.flat));
        }
        return answer(server_mode, server_text);
    }
    const auto global_tool = rules.global;
    const auto preset_tool = rules.preset;
    if (!rules.preset_trusted) {
        // server_mode is already clamped by BuildMcpPolicy.
        PermissionMode mode = global_tool && !preset_spoke_for_server
                                  ? *global_tool
                                  : Fold(global_tool, server_mode).value();
        std::string why = mode == server_mode
                              ? server_text
                              : rule_text("permissions.json", tool_name, mode);
        if (preset_tool && *preset_tool < mode) {
            mode = *preset_tool;
            why = rule_text("project preset", tool_name, mode);
        }
        return answer(mode, std::move(why));
    }
    if (preset_tool) return answer(*preset_tool, rule_text("preset", tool_name, *preset_tool));
    if (global_tool) {
        if (preset_spoke_for_server && server_mode < *global_tool) {
            return answer(server_mode, server_text);
        }
        return answer(*global_tool, rule_text("permissions.json", tool_name, *global_tool));
    }
    return answer(server_mode, server_text);
}

std::shared_ptr<const tools::PermissionModel> MakeMcpPermissionModel() {
    static const auto model = std::make_shared<const McpPermissionModel>();
    return model;
}

tools::BoundPermissionModel BindMcpPermissionModel(McpPermissionPolicy policy) {
    return tools::BoundPermissionModel{
        std::string(kMcpPluginName), MakeMcpPermissionModel(),
        std::make_shared<const McpPermissionPolicy>(std::move(policy))};
}

}  // namespace minimal::mcp
