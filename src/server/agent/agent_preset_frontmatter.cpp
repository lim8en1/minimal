#include "minimal/agent/agent_preset_frontmatter.hpp"

#include "minimal/config/providers_config.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>

#include "minimal/common/logger.hpp"
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

// Booleans accept YAML bools only: a quoted "true" is a mistake worth naming,
// because silently treating it as absent hides that the operator's flag never
// took.
std::optional<bool> OptionalBool(const YAML::Node& node, const std::string& key) {
    const YAML::Node value = node[key];
    if (!value) return std::nullopt;
    if (value.IsScalar()) {
        const std::string text = value.as<std::string>();
        if (text == "true") return true;
        if (text == "false") return false;
    }
    throw std::runtime_error("agent preset: field '" + key + "' must be a boolean");
}

tools::PermissionOverrideLayer DecodePermissionModeBlock(const YAML::Node& node,
                                                         tools::PermissionOverrideLayer layer) {
    const YAML::Node block = node["permission_mode"];
    if (!block || !block.IsMap()) return layer;

    if (block["read_only"]) layer.read_only = config::ParsePermissionMode(block["read_only"].as<std::string>());
    if (block["file_write"]) layer.file_write = config::ParsePermissionMode(block["file_write"].as<std::string>());
    if (block["execute"]) layer.execute = config::ParsePermissionMode(block["execute"].as<std::string>());
    if (block["out_of_workdir"])
        layer.out_of_workdir = config::ParsePermissionMode(block["out_of_workdir"].as<std::string>());
    if (block["network"]) layer.network = config::ParsePermissionMode(block["network"].as<std::string>());

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

// opencode-compat `permission` block, folded into the native layer. Runs BEFORE
// DecodePermissionModeBlock, so a native `permission_mode` field always wins
// over an alias encoding of the same intent.
//
// Keys map onto minimal's categories: edit/write -> file_write, bash ->
// execute, webfetch/websearch -> network, external_directory -> out_of_workdir,
// read/glob/grep/list -> read_only. Pattern-object values collapse: the "*"
// entry when present, otherwise the MOST RESTRICTIVE of the listed actions --
// collapsing `{"git *": allow, "rm *": deny}` to allow would silently widen
// what the author denied, and the pattern layer itself is not built yet.
std::optional<tools::PermissionMode> CollapsePermissionValue(const YAML::Node& value,
                                                             const std::string& key) {
    if (value.IsScalar()) return config::ParsePermissionMode(value.as<std::string>());
    if (value.IsMap()) {
        if (value["*"] && value["*"].IsScalar()) {
            return config::ParsePermissionMode(value["*"].as<std::string>());
        }
        // Most restrictive wins: AlwaysDeny < Ask < AlwaysAllow, so the MINIMUM
        // is the safest collapse of a pattern list.
        std::optional<tools::PermissionMode> most_restrictive;
        for (const auto& entry : value) {
            if (!entry.second.IsScalar()) continue;
            const auto mode = config::ParsePermissionMode(entry.second.as<std::string>());
            if (!most_restrictive || mode < *most_restrictive) most_restrictive = mode;
        }
        return most_restrictive;
    }
    common::DefaultLogger().warn(
        "agent preset: permission.{} is not a mode or a pattern map; ignored", key);
    return std::nullopt;
}

tools::PermissionOverrideLayer DecodePermissionAlias(const YAML::Node& node) {
    tools::PermissionOverrideLayer layer;
    const YAML::Node block = node["permission"];
    if (!block || !block.IsMap()) return layer;

    // Alias key -> the native field it feeds. The layer's members are
    // optionals, so the map holds pointers to the optionals themselves.
    std::unordered_map<std::string, std::optional<tools::PermissionMode>*> by_key{
        {"read", &layer.read_only},   {"glob", &layer.read_only},
        {"grep", &layer.read_only},   {"list", &layer.read_only},
        {"edit", &layer.file_write},  {"write", &layer.file_write},
        {"bash", &layer.execute},
        {"webfetch", &layer.network}, {"websearch", &layer.network},
        {"external_directory", &layer.out_of_workdir},
    };

    for (const auto& entry : block) {
        const std::string key = entry.first.as<std::string>();
        const auto it = by_key.find(key);
        if (it == by_key.end()) {
            // task/skill/question/lsp/doom_loop are opencode keys minimal has
            // no category for; silently dropping a rule the author wrote would
            // WIDEN what they asked for, so it is named instead.
            common::DefaultLogger().warn(
                "agent preset: permission key '{}' has no minimal category; ignored", key);
            continue;
        }
        if (const auto mode = CollapsePermissionValue(entry.second, key)) *it->second = mode;
    }
    return layer;
}

// opencode's deprecated `tools` map (name -> bool), folded into per-tool
// overrides: true -> allow, false -> deny. minimal's allowed_tools is an
// allow-LIST (fail closed), so a deny-by-bool is expressible only through the
// override layer -- which is exactly what opencode's `tools: {write: false}`
// means: everything stays available except the named tool.
void ApplyToolsAlias(const YAML::Node& node, tools::PermissionOverrideLayer& layer) {
    const YAML::Node block = node["tools"];
    if (!block || !block.IsMap()) return;
    for (const auto& entry : block) {
        const std::string tool_name = entry.first.as<std::string>();
        if (tool_name.empty() || tool_name == "*") {
            throw std::runtime_error(
                "agent preset: tools keys must name a tool ('*' would deny-or-allow "
                "everything; use allowed_tools or permission categories)");
        }
        const std::string value = entry.second.as<std::string>();
        if (value == "true") {
            layer.per_tool_overrides[tool_name] = tools::PermissionMode::AlwaysAllow;
        } else if (value == "false") {
            layer.per_tool_overrides[tool_name] = tools::PermissionMode::AlwaysDeny;
        } else {
            throw std::runtime_error("agent preset: tools." + tool_name + " must be true or false");
        }
    }
}

AgentMode DecodeMode(const YAML::Node& node) {
    const YAML::Node value = node["mode"];
    if (!value || !value.IsScalar()) return AgentMode::All;
    const std::string text = value.as<std::string>();
    if (text == "all") return AgentMode::All;
    if (text == "primary") return AgentMode::Primary;
    if (text == "subagent") return AgentMode::Subagent;
    throw std::runtime_error("agent preset: mode must be one of all|primary|subagent, got '" +
                             text + "'");
}

// allowed_skills: a LIST is the native form (every entry = allow); a MAP is the
// opencode-compat form (name -> allow|ask|deny|preload). "*" is legal in both.
std::map<std::string, SkillMode> DecodeAllowedSkills(const YAML::Node& node) {
    std::map<std::string, SkillMode> result;
    const YAML::Node value = node["allowed_skills"];
    if (!value) return result;
    if (value.IsSequence()) {
        for (const auto& item : value) result[item.as<std::string>()] = SkillMode::Allow;
        return result;
    }
    if (value.IsMap()) {
        for (const auto& entry : value) {
            const std::string name = entry.first.as<std::string>();
            const std::string mode = entry.second.as<std::string>();
            if (mode == "allow") {
                result[name] = SkillMode::Allow;
            } else if (mode == "ask") {
                result[name] = SkillMode::Ask;
            } else if (mode == "deny") {
                result[name] = SkillMode::Deny;
            } else if (mode == "preload") {
                result[name] = SkillMode::Preload;
            } else {
                throw std::runtime_error("agent preset: allowed_skills." + name +
                                         " must be one of allow|ask|deny|preload, got '" +
                                         mode + "'");
            }
        }
        return result;
    }
    throw std::runtime_error(
        "agent preset: allowed_skills must be a list of names or a name->mode map");
}

// Structural keys this parser consumes itself. EVERYTHING else (scalars, maps,
// whatever) is passthrough provider-parameter material -- see the class comment.
bool IsKnownKey(const std::string& key) {
    static const std::unordered_map<std::string, char> kKnown{
        {"name", 0},          {"description", 0},   {"model", 0},
        {"allowed_tools", 0}, {"allowed_skills", 0},{"allowed_mcp_servers", 0},
        {"permission_mode", 0}, {"permission", 0},  {"tools", 0},
        {"mcp_permissions", 0}, {"compression_prompt", 0},
        {"mode", 0},          {"disable", 0},       {"hidden", 0},
        {"color", 0},         {"steps", 0},
    };
    return kKnown.count(key) != 0;
}

// Lowering for passthrough parameter values: typed scalars where the YAML type
// survives the trip (bool / int / double), everything else as string. A map or
// sequence lowers recursively. Anything lowerable is carried verbatim, so
// `reasoningEffort: high` arrives as the string the provider expects.
nlohmann::json YamlToJson(const YAML::Node& node) {
    if (node.IsNull()) return nullptr;
    if (node.IsSequence()) {
        nlohmann::json array = nlohmann::json::array();
        for (const auto& item : node) array.push_back(YamlToJson(item));
        return array;
    }
    if (node.IsMap()) {
        nlohmann::json object = nlohmann::json::object();
        for (const auto& entry : node) {
            object[entry.first.as<std::string>()] = YamlToJson(entry.second);
        }
        return object;
    }
    // Scalar. Try typed lowerings in strictness order; each throws
    // BadConversion when the text does not parse as that type.
    try {
        return node.as<bool>();
    } catch (const YAML::BadConversion&) {
    }
    try {
        return static_cast<std::int64_t>(node.as<std::int64_t>());
    } catch (const YAML::BadConversion&) {
    }
    try {
        return node.as<double>();
    } catch (const YAML::BadConversion&) {
    }
    return node.as<std::string>();
}

nlohmann::json DecodeExtraParameters(const YAML::Node& node) {
    nlohmann::json extras = nlohmann::json::object();
    if (!node || !node.IsMap()) return extras;
    for (const auto& entry : node) {
        const std::string key = entry.first.as<std::string>();
        if (IsKnownKey(key)) continue;
        try {
            extras[key] = YamlToJson(entry.second);
        } catch (const std::exception& error) {
            common::DefaultLogger().warn("agent preset: frontmatter key '{}' not carried to "
                                         "provider parameters: {}",
                                         key, error.what());
        }
    }
    return extras;
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
    result.description = OptionalString(node, "description");
    result.model = OptionalString(node, "model");
    // Shape-checked here, existence-checked by AgentPresetRegistry: this file
    // deliberately makes no cross-file lookups, but "local/qwen4" vs "qwen4" is
    // decidable without one, and catching it at load names the preset.
    if (!result.model.empty() && !config::ParseModelRef(result.model)) {
        throw std::runtime_error("agent preset '" + result.name + "': model '" + result.model +
                                 "' must be <provider>/<model>, e.g. local/qwen4");
    }
    result.allowed_tools = OptionalStringList(node, "allowed_tools");
    result.allowed_skills = DecodeAllowedSkills(node);
    result.allowed_mcp_servers = OptionalStringList(node, "allowed_mcp_servers");
    // Alias first, native second: `permission_mode` always wins over an alias
    // encoding of the same intent.
    result.permission_mode = DecodePermissionAlias(node);
    result.permission_mode = DecodePermissionModeBlock(node, std::move(result.permission_mode));
    ApplyToolsAlias(node, result.permission_mode);
    result.mcp_permissions = DecodeMcpPermissionsBlock(node);
    result.compression_prompt = OptionalString(node, "compression_prompt");

    // --- opencode-compat surface ----------------------------------------------
    result.mode = DecodeMode(node);
    if (const auto disable = OptionalBool(node, "disable")) result.disable = *disable;
    if (const auto hidden = OptionalBool(node, "hidden")) result.hidden = *hidden;
    result.color = OptionalString(node, "color");
    const YAML::Node steps = node["steps"];
    if (steps) {
        if (!steps.IsScalar() || steps.as<std::string>().find_first_not_of("0123456789") !=
                                     std::string::npos) {
            throw std::runtime_error("agent preset: steps must be a non-negative integer");
        }
        result.steps = steps.as<int>();
    }
    result.extra_parameters = DecodeExtraParameters(node);

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
