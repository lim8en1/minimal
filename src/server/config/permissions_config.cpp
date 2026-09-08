#include "minimal/config/permissions_config.hpp"

#include <stdexcept>

#include "minimal/config/json_config_loader.hpp"
#include "minimal/config/permission_codec.hpp"

namespace minimal::config {

namespace {

void ParseFallback(const nlohmann::json& doc, tools::PermissionOverrideLayer& layer) {
    if (!doc.contains("fallback")) return;
    const auto& fallback = doc.at("fallback");
    if (fallback.contains("read_only"))
        layer.read_only = ParsePermissionMode(fallback.at("read_only").get<std::string>());
    if (fallback.contains("file_write"))
        layer.file_write = ParsePermissionMode(fallback.at("file_write").get<std::string>());
    if (fallback.contains("execute"))
        layer.execute = ParsePermissionMode(fallback.at("execute").get<std::string>());
    if (fallback.contains("out_of_workdir"))
        layer.out_of_workdir = ParsePermissionMode(fallback.at("out_of_workdir").get<std::string>());
    if (fallback.contains("network"))
        layer.network = ParsePermissionMode(fallback.at("network").get<std::string>());
}

void ParsePerToolOverrides(const nlohmann::json& doc, tools::PermissionOverrideLayer& layer) {
    if (!doc.contains("per_tool_overrides")) return;
    for (const auto& [key, value] : doc.at("per_tool_overrides").items()) {
        if (key.empty()) {
            throw std::runtime_error("permissions.json: per_tool_overrides has an empty-string key");
        }
        layer.per_tool_overrides[key] = ParsePermissionMode(value.get<std::string>());
    }
}

void ParsePatternOverrides(const nlohmann::json& doc, tools::PermissionOverrideLayer& layer) {
    if (!doc.contains("pattern_overrides")) return;
    const auto& patterns = doc.at("pattern_overrides");
    if (!patterns.is_object()) {
        throw std::runtime_error("permissions.json: pattern_overrides must be an object");
    }
    for (const auto& [tool, rules] : patterns.items()) {
        if (tool.empty()) {
            throw std::runtime_error("permissions.json: pattern_overrides has an empty-string key");
        }
        if (!rules.is_object()) {
            throw std::runtime_error("permissions.json: pattern_overrides." + tool +
                                     " must be an object of {glob: mode}");
        }
        auto& out = layer.pattern_overrides[tool];
        for (const auto& [glob, mode] : rules.items()) {
            if (glob.empty()) {
                throw std::runtime_error("permissions.json: pattern_overrides." + tool +
                                         " has an empty glob");
            }
            out.push_back({glob, ParsePermissionMode(mode.get<std::string>())});
        }
    }
}

void ParseExternalDirectoryPatterns(const nlohmann::json& doc,
                                    tools::PermissionOverrideLayer& layer) {
    if (!doc.contains("external_directory_patterns")) return;
    const auto& patterns = doc.at("external_directory_patterns");
    if (!patterns.is_object()) {
        throw std::runtime_error(
            "permissions.json: external_directory_patterns must be an object of {glob: mode}");
    }
    for (const auto& [glob, mode] : patterns.items()) {
        if (glob.empty()) {
            throw std::runtime_error("permissions.json: external_directory_patterns has an empty glob");
        }
        layer.out_of_workdir_patterns.push_back({glob, ParsePermissionMode(mode.get<std::string>())});
    }
}

}  // namespace

PermissionsConfig ParsePermissionsConfig(const nlohmann::json& doc) {
    PermissionsConfig config;
    try {
        if (doc.contains("default_run_mode")) {
            config.default_run_mode = ParsePermissionRunMode(doc.at("default_run_mode").get<std::string>());
        }
        ParseFallback(doc, config.global_layer);
        ParsePerToolOverrides(doc, config.global_layer);
        ParsePatternOverrides(doc, config.global_layer);
        ParseExternalDirectoryPatterns(doc, config.global_layer);
        if (doc.contains("mcp_default_mode")) {
            config.mcp_default_mode = ParsePermissionMode(doc.at("mcp_default_mode").get<std::string>());
        }
        if (doc.contains("advisor")) {
            config.advisor = doc.at("advisor").get<std::string>();
        }
        if (doc.contains("advisor_inherit")) {
            config.advisor_inherit = doc.at("advisor_inherit").get<bool>();
        }
        if (doc.contains("advisor_timeout_ms")) {
            const auto value = doc.at("advisor_timeout_ms").get<int>();
            if (value < 0) {
                throw std::runtime_error(
                    "permissions.json: advisor_timeout_ms must be >= 0 (0 disables the budget)");
            }
            config.advisor_timeout_ms = value;
        }
    } catch (const nlohmann::json::exception& error) {
        throw std::runtime_error(std::string("permissions.json: ") + error.what());
    }
    return config;
}

PermissionsConfig LoadPermissionsConfig(const std::filesystem::path& path) {
    return LoadJsonConfig<PermissionsConfig>(path, "permissions", AbsencePolicy::kThrow,
                                             EmptyFilePolicy::kTreatAsMalformed,
                                             ParsePermissionsConfig);
}

}  // namespace minimal::config
