#include "minimal/agent/agent_preset_frontmatter.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>

#include "minimal/config/permission_codec.hpp"

namespace minimal::agent {

namespace {

std::string RequireString(const YAML::Node& node, const std::string& key) {
    const YAML::Node value = node[key];
    if (!value || !value.IsScalar()) {
        throw std::runtime_error("agent preset: missing required field '" + key + "'");
    }
    return value.as<std::string>();
}

std::string OptionalString(const YAML::Node& node, const std::string& key, std::string fallback = "") {
    const YAML::Node value = node[key];
    if (!value || !value.IsScalar()) return fallback;
    return value.as<std::string>();
}

std::vector<std::string> OptionalStringList(const YAML::Node& node, const std::string& key) {
    std::vector<std::string> result;
    const YAML::Node value = node[key];
    if (!value || !value.IsSequence()) return result;
    for (const auto& item : value) result.push_back(item.as<std::string>());
    return result;
}

tools::PermissionOverrideLayer DecodePermissionModeBlock(const YAML::Node& node) {
    tools::PermissionOverrideLayer layer;
    const YAML::Node block = node["permission_mode"];
    if (!block || !block.IsMap()) return layer;

    if (block["read_only"]) layer.read_only = config::ParsePermissionMode(block["read_only"].as<std::string>());
    if (block["file_write"]) layer.file_write = config::ParsePermissionMode(block["file_write"].as<std::string>());
    if (block["execute"]) layer.execute = config::ParsePermissionMode(block["execute"].as<std::string>());
    if (block["out_of_workdir"])
        layer.out_of_workdir = config::ParsePermissionMode(block["out_of_workdir"].as<std::string>());

    const YAML::Node overrides = block["overrides"];
    if (overrides && overrides.IsMap()) {
        for (const auto& entry : overrides) {
            const std::string tool_name = entry.first.as<std::string>();
            if (tool_name.empty()) {
                throw std::runtime_error("agent preset: permission_mode.overrides has an empty-string key");
            }
            layer.per_tool_overrides[tool_name] = config::ParsePermissionMode(entry.second.as<std::string>());
        }
    }
    return layer;
}

std::unordered_map<std::string, tools::PermissionMode> DecodeMcpPermissionsBlock(const YAML::Node& node) {
    std::unordered_map<std::string, tools::PermissionMode> result;
    const YAML::Node block = node["mcp_permissions"];
    if (!block || !block.IsMap()) return result;
    for (const auto& entry : block) {
        result[entry.first.as<std::string>()] = config::ParsePermissionMode(entry.second.as<std::string>());
    }
    return result;
}

}  // namespace

AgentPresetFrontmatter ParseAgentPresetFrontmatter(const config::ParsedFrontmatter& parsed) {
    const YAML::Node& node = parsed.frontmatter;

    AgentPresetFrontmatter result;
    result.name = RequireString(node, "name");
    result.provider = RequireString(node, "provider");
    result.description = OptionalString(node, "description");
    result.model = OptionalString(node, "model");
    result.allowed_tools = OptionalStringList(node, "allowed_tools");
    result.allowed_skills = OptionalStringList(node, "allowed_skills");
    result.allowed_mcp_servers = OptionalStringList(node, "allowed_mcp_servers");
    result.permission_mode = DecodePermissionModeBlock(node);
    result.mcp_permissions = DecodeMcpPermissionsBlock(node);
    result.compression_prompt = OptionalString(node, "compression_prompt");
    result.body = parsed.body;
    return result;
}

AgentPresetFrontmatter LoadAgentPresetFrontmatter(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("agent preset not found: " + path.string());
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return ParseAgentPresetFrontmatter(config::parse_frontmatter_markdown(buffer.str()));
}

}  // namespace minimal::agent
