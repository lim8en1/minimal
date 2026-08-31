#include "minimal/config/permissions_config.hpp"

#include <fstream>
#include <stdexcept>

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

}  // namespace

PermissionsConfig ParsePermissionsConfig(const nlohmann::json& doc) {
    PermissionsConfig config;
    try {
        if (doc.contains("default_run_mode")) {
            config.default_run_mode = ParsePermissionRunMode(doc.at("default_run_mode").get<std::string>());
        }
        ParseFallback(doc, config.global_layer);
        ParsePerToolOverrides(doc, config.global_layer);
        if (doc.contains("mcp_default_mode")) {
            config.mcp_default_mode = ParsePermissionMode(doc.at("mcp_default_mode").get<std::string>());
        }
        if (doc.contains("advisor")) {
            config.advisor = doc.at("advisor").get<std::string>();
        }
        if (doc.contains("advisor_inherit")) {
            config.advisor_inherit = doc.at("advisor_inherit").get<bool>();
        }
    } catch (const nlohmann::json::exception& error) {
        throw std::runtime_error(std::string("permissions.json: ") + error.what());
    }
    return config;
}

PermissionsConfig LoadPermissionsConfig(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("permissions.json not found: " + path.string());
    nlohmann::json doc;
    try {
        in >> doc;
    } catch (const nlohmann::json::exception& error) {
        throw std::runtime_error("permissions.json: invalid JSON: " + std::string(error.what()));
    }
    return ParsePermissionsConfig(doc);
}

}  // namespace minimal::config
