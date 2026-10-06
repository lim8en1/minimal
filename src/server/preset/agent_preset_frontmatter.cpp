#include "minimal/agent/agent_preset_frontmatter.hpp"

#include "minimal/config/providers_config.hpp"

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <unordered_map>

#include "minimal/common/logger.hpp"
#include "minimal/common/read_file.hpp"
#include "minimal/config/permission_codec.hpp"
#include "minimal/common/units.hpp"

namespace minimal::agent {

namespace {

// A preset is prose plus a handful of frontmatter keys -- kilobytes, not
// megabytes. This is the cap, not a tuning knob: agents/*.md is UNTRUSTED
// content (a project-local file the current repo shipped, walked before the
// operator's own config_dir -- see AgentPresetRegistry::ResolveUncached),
// and yaml-cpp's recursive-descent parser has no built-in depth or size
// limit. An oversized file is refused after reading at most one byte past
// the cap, before any parse, on every ListAvailable() scan and Resolve().
constexpr std::uintmax_t kMaxPresetFileBytes = common::kMiB;

std::string RequireString(const YAML::Node& node, const std::string& key) {
    const YAML::Node value = node[key];
    if (!value || !value.IsScalar()) {
        throw std::runtime_error("agent preset: missing required field '" + key + "'");
    }
    return value.as<std::string>();
}

std::string OptionalString(const YAML::Node& node, const std::string& key, std::string fallback = "") {
    const YAML::Node value = node[key];
    if (!value) return fallback;
    if (!value.IsScalar()) {
        throw std::runtime_error("agent preset: field '" + key + "' must be a single value");
    }
    return value.as<std::string>();
}

// A present non-list is an error, not "no entries": `allowed_tools: "*"` or
// `allowed_tools: read, grep` used to read as an empty list -- a preset that
// silently served zero tools.
std::vector<std::string> OptionalStringList(const YAML::Node& node, const std::string& key) {
    std::vector<std::string> result;
    const YAML::Node value = node[key];
    if (!value) return result;
    if (!value.IsSequence()) {
        throw std::runtime_error("agent preset: field '" + key +
                                 "' must be a list (e.g. " + key + ": [a, b])");
    }
    for (const auto& item : value) {
        if (!item.IsScalar() || item.as<std::string>().empty()) {
            throw std::runtime_error("agent preset: field '" + key +
                                     "' must contain non-empty names");
        }
        result.push_back(item.as<std::string>());
    }
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

// A scalar at `path` (a mode name, a precedence name), or a named error: yaml-cpp's
// own "bad conversion" names neither the field nor the expected shape.
std::string ScalarAt(const YAML::Node& value, const std::string& path, const char* expected) {
    if (!value.IsScalar()) {
        throw std::runtime_error("agent preset: " + path + " must be " + expected + ", got a " +
                                 (value.IsMap() ? "map" : value.IsSequence() ? "list" : "null"));
    }
    return value.as<std::string>();
}

constexpr const char* kModeShape = "allow, ask or deny";

tools::PermissionOverrideLayer DecodePermissionModeBlock(const YAML::Node& node,
                                                         tools::PermissionOverrideLayer layer) {
    const YAML::Node block = node["permission_mode"];
    if (!block) return layer;
    if (!block.IsMap()) {
        throw std::runtime_error("agent preset: permission_mode must be a map");
    }
    const auto mode_at = [&](const char* key) {
        return config::ParsePermissionMode(
            ScalarAt(block[key], std::string("permission_mode.") + key, kModeShape));
    };

    if (block["read_only"]) layer.read_only = mode_at("read_only");
    if (block["file_write"]) layer.file_write = mode_at("file_write");
    if (block["execute"]) layer.execute = mode_at("execute");
    if (block["out_of_workdir"]) layer.out_of_workdir = mode_at("out_of_workdir");
    if (block["network"]) layer.network = mode_at("network");
    if (block["pattern_precedence"]) {
        layer.pattern_precedence = config::ParsePatternPrecedence(ScalarAt(
            block["pattern_precedence"], "permission_mode.pattern_precedence", "a string"));
    }

    const YAML::Node overrides = block["overrides"];
    if (overrides && !overrides.IsMap()) {
        throw std::runtime_error("agent preset: permission_mode.overrides must be a map of {tool: mode}");
    }
    if (overrides && overrides.IsMap()) {
        for (const auto& entry : overrides) {
            const std::string tool_name = entry.first.as<std::string>();
            if (tool_name.empty()) {
                throw std::runtime_error("agent preset: permission_mode.overrides has an empty-string key");
            }
            if (entry.second.IsMap()) {
                throw std::runtime_error(
                    "agent preset: permission_mode.overrides." + tool_name +
                    " must be allow, ask or deny; {glob: mode} rules go under "
                    "permission_mode.patterns." + tool_name);
            }
            layer.per_tool_overrides[tool_name] = config::ParsePermissionMode(
                ScalarAt(entry.second, "permission_mode.overrides." + tool_name, kModeShape));
        }
    }

    // The native pattern surface, shaped like permissions.json's
    // pattern_overrides: {tool: {glob: mode}}. The alias block above is the
    // opencode spelling of the same layer.
    const YAML::Node patterns = block["patterns"];
    if (patterns && !patterns.IsMap()) {
        throw std::runtime_error(
            "agent preset: permission_mode.patterns must be a map of {tool: {glob: mode}}");
    }
    if (patterns && patterns.IsMap()) {
        for (const auto& entry : patterns) {
            const std::string tool_name = entry.first.as<std::string>();
            if (tool_name.empty()) {
                throw std::runtime_error("agent preset: permission_mode.patterns has an empty-string key");
            }
            if (!entry.second.IsMap()) {
                throw std::runtime_error("agent preset: permission_mode.patterns." + tool_name +
                                         " must be a map of {glob: mode}");
            }
            auto& rules = layer.pattern_overrides[tool_name];
            for (const auto& rule : entry.second) {
                const std::string glob = rule.first.as<std::string>();
                if (glob.empty()) {
                    throw std::runtime_error("agent preset: permission_mode.patterns." +
                                             tool_name + " has an empty glob");
                }
                rules.push_back({glob, config::ParsePermissionMode(ScalarAt(
                                           rule.second,
                                           "permission_mode.patterns." + tool_name + "[\"" +
                                               glob + "\"]",
                                           kModeShape))});
            }
        }
    }
    return layer;
}

// The `permission` block (opencode's spelling), folded into the native layer.
// Runs BEFORE DecodePermissionModeBlock, so a native `permission_mode` field
// always wins over an alias encoding of the same intent.
//
// A key naming ONE tool -- a live tool's permission key, or a spelling the
// tool itself declared (Tool::config_aliases: tools-web answers "webfetch")
// -- governs that tool only: a scalar becomes a per-tool override. Mapping
// tool words onto categories made `bash: allow` allow every Execute tool and
// `read: deny` deny every read-only one. `edit`/`write` feed file_write (they
// govern every file-modifying tool); external_directory -> out_of_workdir. A
// MAP value under a tool key is the pattern layer, unless the tool declares
// it takes no pattern rules (Tool::accepts_pattern_rules) -- then the map
// collapses to its most restrictive mode. Core names no tool here.
std::optional<tools::PermissionMode> CollapsePermissionValue(const YAML::Node& value,
                                                             const std::string& key) {
    if (value.IsScalar()) return config::ParsePermissionMode(value.as<std::string>());
    if (value.IsMap()) {
        // "*" participates in the same most-restrictive scan as every other
        // pattern -- the loop below already reads it. An early return here
        // would let `{"*": allow, "sudo *": deny}` collapse to allow,
        // contradicting this function's own contract: a listed deny is an
        // author's intent, and collapsing it away silently widens it.
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

// True for alias keys that are also a LIVE registered tool's permission key,
// so a map value has a subject to match against and becomes real pattern
// rules -- checked against the actual ToolRegistry vocabulary
// (AgentPresetRegistry::ToolPermissionKeys::all), not a fixed literal list,
// so a renamed/aliased/third-party tool name works here exactly like
// tools-fs's bundled ones.
bool AliasKeyIsToolName(const std::string& key,
                        const std::vector<std::string>& known_tool_permission_keys) {
    return std::find(known_tool_permission_keys.begin(), known_tool_permission_keys.end(), key) !=
           known_tool_permission_keys.end();
}

void DecodeToolPatternMap(const YAML::Node& value, const std::string& key,
                          tools::PermissionOverrideLayer& layer) {
    auto& rules = layer.pattern_overrides[key];
    for (const auto& entry : value) {
        const std::string glob = entry.first.as<std::string>();
        if (glob.empty()) {
            throw std::runtime_error("agent preset: permission." + key + " has an empty pattern");
        }
        rules.push_back({glob, config::ParsePermissionMode(entry.second.as<std::string>())});
    }
}

tools::PermissionOverrideLayer DecodePermissionAlias(const YAML::Node& node,
                                                    const ToolVocabulary& tools) {
    tools::PermissionOverrideLayer layer;
    const YAML::Node block = node["permission"];
    if (!block || !block.IsMap()) return layer;

    // Alias key -> the native CATEGORY field it feeds (file-modification only).
    std::unordered_map<std::string, std::optional<tools::PermissionMode>*> by_key{
        {"edit", &layer.file_write},
        {"write", &layer.file_write},
        {"external_directory", &layer.out_of_workdir},
    };
    const auto is_live = [&tools](const std::string& key) {
        return AliasKeyIsToolName(key, tools.permission_keys);
    };

    for (const auto& entry : block) {
        const std::string key = entry.first.as<std::string>();
        // A key naming ONE tool -- a live tool's permission key, or a spelling
        // the tool declared (Tool::config_aliases) -- governs that tool only.
        // The category words above stay categories even when a tool shares
        // the name; a pattern map under one still reaches that tool below.
        std::optional<std::string> tool_key;
        if (const auto alias = tools.config_aliases.find(key); alias != tools.config_aliases.end()) {
            tool_key = alias->second;
        } else if (!by_key.count(key) && is_live(key)) {
            tool_key = key;
        }
        if (tool_key) {
            const bool takes_patterns =
                std::find(tools.no_pattern_rules.begin(), tools.no_pattern_rules.end(),
                          *tool_key) == tools.no_pattern_rules.end();
            if (entry.second.IsMap() && takes_patterns) {
                DecodeToolPatternMap(entry.second, *tool_key, layer);
            } else if (const auto mode = CollapsePermissionValue(entry.second, key)) {
                layer.per_tool_overrides[*tool_key] = *mode;
            }
            continue;
        }
        const auto it = by_key.find(key);
        if (it == by_key.end()) {
            // Neither a category word nor a tool this server has (task, skill,
            // lsp, a disabled plugin's tool): silently dropping a rule the
            // author wrote would WIDEN what they asked for, so it is named.
            common::DefaultLogger().warn(
                "agent preset: permission key '{}' names no category or loaded tool; ignored", key);
            continue;
        }
        if (key == "external_directory" && entry.second.IsMap()) {
            // Each entry is a path pattern on the out_of_workdir dimension:
            // matched against the canonical absolute path of an invocation
            // that resolved outside the project root.
            for (const auto& rule : entry.second) {
                const std::string glob = rule.first.as<std::string>();
                if (glob.empty()) {
                    throw std::runtime_error(
                        "agent preset: permission.external_directory has an empty pattern");
                }
                layer.out_of_workdir_patterns.push_back(
                    {glob, config::ParsePermissionMode(rule.second.as<std::string>())});
            }
            continue;
        }
        if (is_live(key) && entry.second.IsMap()) {
            DecodeToolPatternMap(entry.second, key, layer);
            continue;
        }
        // Two alias keys collapse onto one category (edit/write ->
        // file_write), and YAML document order is otherwise arbitrary: applying
        // the keys in sequence would let a later permissive key overwrite an
        // earlier deny and WIDEN what the author asked for. Most restrictive
        // wins (AlwaysDeny < Ask < AlwaysAllow), matching the collapse of a
        // pattern list inside one value.
        if (const auto mode = CollapsePermissionValue(entry.second, key)) {
            if (!*it->second || mode < *it->second) *it->second = mode;
        }
    }
    return layer;
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

// Structural keys this parser consumes itself.
bool IsKnownKey(const std::string& key) {
    static const std::unordered_map<std::string, char> kKnown{
        {"name", 0},          {"description", 0},   {"model", 0},
        {"allowed_tools", 0}, {"tool_sources", 0}, {"plugins", 0},
        {"allowed_subagents", 0},
        {"permission_mode", 0}, {"permission", 0},
        {"compression_prompt", 0}, {"compaction_strategy", 0},
        {"mode", 0},          {"disable", 0},       {"hidden", 0},
        {"color", 0},         {"steps", 0},         {"tool_config", 0},
        {"plugin_permissions", 0}, {"parameters", 0},
    };
    return kKnown.count(key) != 0;
}

// Provider parameters an opencode agent file writes at the top level. Kept so
// those files load unchanged; every other provider parameter goes under
// `parameters:`, and any other key is an error -- a typo'd structural key
// (`allowed-tools`) must not ride to the provider as a request field.
bool IsTopLevelParameter(const std::string& key) {
    return key == "temperature" || key == "top_p" || key == "reasoningEffort";
}

// Lowering for passthrough parameter values: typed scalars where the YAML type
// survives the trip (bool / int / double), everything else as string. A
// QUOTED scalar always stays a string (`user: "007"` is not 7). A map or
// sequence lowers recursively.
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
    // yaml-cpp tags a quoted scalar "!" and a plain one "?".
    if (node.Tag() == "!") return node.as<std::string>();
    // Plain scalar: YAML 1.2 core booleans only (yaml-cpp's as<bool> also
    // takes yes/no/on/off/y/n, which turned `stop: no` into false).
    const std::string text = node.as<std::string>();
    if (text == "true") return true;
    if (text == "false") return false;
    try {
        return static_cast<std::int64_t>(node.as<std::int64_t>());
    } catch (const YAML::BadConversion&) {
    }
    try {
        return node.as<double>();
    } catch (const YAML::BadConversion&) {
    }
    return text;
}

// Fields earlier formats used, with what replaced them: a file written for
// that format gets the migration in the error, not a hint about parameters:.
const char* ReplacementFor(const std::string& key) {
    static const std::unordered_map<std::string, const char*> kReplaced{
        {"tools", "list the tools in allowed_tools: [...] and gate them with permission_mode"},
        {"allowed_mcp_servers", "use tool_sources: {mcp: [<server>, ...]}"},
        {"mcp_servers", "use tool_sources: {mcp: [<server>, ...]}"},
        {"mcp_permissions", "use plugin_permissions: {mcp: {servers: {<server>: <mode>}}}"},
        {"allowed_skills", "use plugin_permissions: {skills: [...] or {<skill>: <mode>}}"},
        {"skills", "use plugin_permissions: {skills: [...] or {<skill>: <mode>}}"},
    };
    const auto it = kReplaced.find(key);
    return it == kReplaced.end() ? nullptr : it->second;
}

// Every frontmatter key is known, an allowlisted top-level parameter, or an
// error naming the fix. All unknown keys are reported at once.
void RejectUnknownKeys(const YAML::Node& node) {
    if (!node || !node.IsMap()) return;
    std::string problems;
    bool any_plain = false;
    for (const auto& entry : node) {
        const std::string key = entry.first.as<std::string>();
        if (IsKnownKey(key) || IsTopLevelParameter(key) || key.rfind("//", 0) == 0) continue;
        problems += problems.empty() ? "" : "; ";
        if (const char* replacement = ReplacementFor(key)) {
            problems += "'" + key + "' was removed -- " + replacement;
        } else {
            problems += "unknown field '" + key + "'";
            any_plain = true;
        }
    }
    if (problems.empty()) return;
    if (any_plain) problems += " (provider request parameters go under parameters:)";
    throw std::runtime_error("agent preset: " + problems);
}

// The provider parameters this preset sets: the allowlisted top-level keys,
// then the `parameters:` map over them.
nlohmann::json DecodeExtraParameters(const YAML::Node& node) {
    nlohmann::json extras = nlohmann::json::object();
    if (!node || !node.IsMap()) return extras;
    for (const auto& entry : node) {
        const std::string key = entry.first.as<std::string>();
        if (IsTopLevelParameter(key)) extras[key] = YamlToJson(entry.second);
    }
    if (const YAML::Node parameters = node["parameters"]) {
        if (!parameters.IsMap()) {
            throw std::runtime_error("agent preset: parameters must be a map of request fields");
        }
        for (const auto& entry : parameters) {
            const std::string key = entry.first.as<std::string>();
            // Request fields the provider builders own: a parameter spelled
            // like one would fight the structural field it names.
            if (key == "tools" || key == "tool_choice" || key == "messages" || key == "stream" ||
                key == "model") {
                throw std::runtime_error("agent preset: parameters." + key +
                                         " is a request field the harness owns");
            }
            extras[key] = YamlToJson(entry.second);
        }
    }
    return extras;
}

// tool_config: {<namespace>: {...}} -- per-tool plugin configuration, carried
// verbatim (YamlToJson lowering, same as extra_parameters). Structural, so it
// never reaches the provider; the namespace keys are collision-checked at
// startup against what registered tools claim (see sanity_check).
nlohmann::json DecodeToolConfig(const YAML::Node& node) {
    const YAML::Node value = node["tool_config"];
    if (!value) return nlohmann::json::object();
    if (!value.IsMap()) {
        throw std::runtime_error(
            "agent preset: tool_config must be a map of {namespace: {...}}");
    }
    nlohmann::json result = nlohmann::json::object();
    for (const auto& entry : value) {
        const std::string ns = entry.first.as<std::string>();
        if (ns.empty()) {
            throw std::runtime_error(
                "agent preset: tool_config has an empty-string namespace key");
        }
        result[ns] = YamlToJson(entry.second);
    }
    return result;
}

void AppendUnique(std::vector<std::string>& out, const std::vector<std::string>& items) {
    for (const auto& item : items) {
        if (std::find(out.begin(), out.end(), item) == out.end()) out.push_back(item);
    }
}

// tool_sources: {<source>: [members]}.
std::map<std::string, std::vector<std::string>> DecodeToolSources(const YAML::Node& node) {
    std::map<std::string, std::vector<std::string>> result;
    if (const YAML::Node value = node["tool_sources"]) {
        if (!value.IsMap()) {
            throw std::runtime_error(
                "agent preset: tool_sources must be a map of {source: [members]}");
        }
        for (const auto& entry : value) {
            const auto source = entry.first.as<std::string>();
            if (source.empty() || !entry.second.IsSequence()) {
                throw std::runtime_error("agent preset: tool_sources entry '" + source +
                                         "' must be a named list of members");
            }
            std::vector<std::string> members;
            for (const auto& item : entry.second) members.push_back(item.as<std::string>());
            AppendUnique(result[source], members);
        }
    }
    return result;
}

// plugin_permissions: {<model>: {...}} -- carried verbatim like tool_config;
// only the outer shape is core's to check.
nlohmann::json DecodePluginPermissions(const YAML::Node& node) {
    nlohmann::json result = nlohmann::json::object();
    const YAML::Node value = node["plugin_permissions"];
    if (!value) return result;
    if (!value.IsMap()) {
        throw std::runtime_error(
            "agent preset: plugin_permissions must be a map of {model: {...}}");
    }
    for (const auto& entry : value) {
        const auto name = entry.first.as<std::string>();
        if (name.empty()) {
            throw std::runtime_error(
                "agent preset: plugin_permissions has an empty-string model key");
        }
        result[name] = YamlToJson(entry.second);
    }
    return result;
}


}  // namespace

AgentPresetFrontmatter ParseAgentPresetFrontmatter(
    const config::ParsedFrontmatter& parsed, const ToolVocabulary& tools) {
    const YAML::Node& node = parsed.frontmatter;

    AgentPresetFrontmatter result;
    RejectUnknownKeys(node);
    result.name = RequireString(node, "name");
    result.description = OptionalString(node, "description");
    result.model = OptionalString(node, "model");
    // Shape-checked here, existence-checked by AgentPresetRegistry: this file
    // deliberately makes no cross-file lookups, but "local/qwen4" vs "qwen4" is
    // decidable without one, and catching it at load names the preset.
    if (!result.model.empty() && !config::SplitModelRef(result.model)) {
        throw std::runtime_error("agent preset '" + result.name + "': model '" + result.model +
                                 "' must be <provider>/<model>, e.g. local/qwen4");
    }
    result.allowed_tools = OptionalStringList(node, "allowed_tools");
    result.plugins = OptionalStringList(node, "plugins");
    result.tool_sources = DecodeToolSources(node);
    // Present-vs-absent is semantic here (absent = no restriction, present =
    // the exact allowlist), so this cannot go through OptionalStringList,
    // whose empty result is ambiguous. An empty entry would silently match
    // nothing-or-everything depending on downstream comparison, so it is
    // named instead.
    if (const YAML::Node value = node["allowed_subagents"]) {
        if (!value.IsSequence()) {
            throw std::runtime_error(
                "agent preset: allowed_subagents must be a list of preset names");
        }
        std::vector<std::string> list;
        for (const auto& item : value) {
            const std::string name = item.as<std::string>();
            if (name.empty()) {
                throw std::runtime_error(
                    "agent preset: allowed_subagents has an empty entry");
            }
            list.push_back(name);
        }
        result.allowed_subagents = std::move(list);
    }
    // Alias first, native second: `permission_mode` -- categories AND per-tool
    // overrides -- always wins over an alias encoding of the same intent. Both
    // aliases feed the layer BEFORE the native block decodes into it, so a
    // native field or override replaces what an alias wrote; running the
    // `tools` alias after the block would let it overwrite a native
    // permission_mode.overrides entry, inverting this contract.
    result.permission_mode = DecodePermissionAlias(node, tools);
    result.permission_mode = DecodePermissionModeBlock(node, std::move(result.permission_mode));
    if (const YAML::Node block = node["permission_mode"]; block && block.IsMap()) {
        static const std::vector<std::string> kKnownPermissionKeys{
            "read_only", "file_write", "execute", "out_of_workdir", "network",
            "overrides", "patterns", "pattern_precedence"};
        for (const auto& entry : block) {
            const std::string key = entry.first.as<std::string>();
            if (std::find(kKnownPermissionKeys.begin(), kKnownPermissionKeys.end(), key) ==
                kKnownPermissionKeys.end()) {
                // A typo'd permission rule silently ignored is less
                // protection than written.
                throw std::runtime_error("agent preset: unknown permission_mode field '" + key +
                                         "'");
            }
        }
    } else if (block) {
        throw std::runtime_error("agent preset: permission_mode must be a map");
    }
    result.plugin_permissions = DecodePluginPermissions(node);
    result.compression_prompt = OptionalString(node, "compression_prompt");
    result.compaction_strategy = OptionalString(node, "compaction_strategy");

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
    result.tool_config = DecodeToolConfig(node);

    result.body = parsed.body;
    return result;
}

AgentPresetFrontmatter LoadAgentPresetFrontmatter(const std::filesystem::path& path,
                                                  const ToolVocabulary& tools) {
    common::ReadFileResult file = common::ReadFile(path, kMaxPresetFileBytes);
    if (file.status == common::ReadFileResult::Status::kNotFound) {
        throw std::runtime_error("agent preset not found: " + path.string());
    }
    if (!file.ok()) throw std::runtime_error("agent preset: " + file.error);
    return ParseAgentPresetFrontmatter(config::parse_frontmatter_markdown(file.contents), tools);
}

}  // namespace minimal::agent
