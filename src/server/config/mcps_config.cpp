#include "minimal/config/mcps_config.hpp"

#include <fstream>
#include <stdexcept>
#include <unordered_set>

namespace minimal::config {

namespace {

std::string EntryLabel(const nlohmann::json& entry, std::size_t index) {
    // Name first so the diagnostic reads like the other config loaders'; fall
    // back to the position for an entry whose name is itself the problem.
    if (entry.contains("name") && entry.at("name").is_string() &&
        !entry.at("name").get<std::string>().empty()) {
        return "entry '" + entry.at("name").get<std::string>() + "'";
    }
    return "entry #" + std::to_string(index);
}

std::unordered_map<std::string, std::string> ParseStringMap(const nlohmann::json& doc,
                                                            const char* field) {
    std::unordered_map<std::string, std::string> out;
    if (!doc.is_object()) {
        throw std::runtime_error(std::string(field) + " must be an object of strings");
    }
    for (const auto& [key, value] : doc.items()) {
        if (!value.is_string()) {
            throw std::runtime_error(std::string(field) + " value for '" + key + "' must be a string");
        }
        out.emplace(key, value.get<std::string>());
    }
    return out;
}

McpServerEntry ParseEntry(const nlohmann::json& entry, std::size_t index) {
    if (!entry.is_object()) throw std::runtime_error("must be an object");

    McpServerEntry result;
    result.name = entry.at("name").get<std::string>();
    if (result.name.empty()) throw std::runtime_error("name must be a non-empty string");

    const std::string transport = entry.at("transport").get<std::string>();
    if (transport == "stdio") {
        result.transport = McpTransport::Stdio;
        result.command = entry.at("command").get<std::string>();
        if (result.command.empty()) throw std::runtime_error("stdio entry needs a non-empty command");
        if (entry.contains("args")) {
            for (const auto& arg : entry.at("args")) {
                if (!arg.is_string()) throw std::runtime_error("args must be an array of strings");
                result.args.push_back(arg.get<std::string>());
            }
        }
        if (entry.contains("env")) result.env = ParseStringMap(entry.at("env"), "env");
    } else if (transport == "http") {
        result.transport = McpTransport::Http;
        result.url = entry.at("url").get<std::string>();
        if (result.url.empty()) throw std::runtime_error("http entry needs a non-empty url");
        if (entry.contains("headers")) result.headers = ParseStringMap(entry.at("headers"), "headers");
    } else {
        throw std::runtime_error("transport must be \"stdio\" or \"http\", got '" + transport + "'");
    }

    result.enabled = entry.value("enabled", result.enabled);
    return result;
}

}  // namespace

const McpServerEntry* McpsConfig::Find(const std::string& name) const {
    for (const auto& entry : entries) {
        if (entry.name == name) return &entry;
    }
    return nullptr;
}

McpsConfig ParseMcpsConfig(const nlohmann::json& doc) {
    if (!doc.is_array()) throw std::runtime_error("mcps.json: expected a top-level array");

    McpsConfig config;
    std::unordered_set<std::string> seen_names;
    for (std::size_t i = 0; i < doc.size(); ++i) {
        try {
            McpServerEntry entry = ParseEntry(doc.at(i), i);
            if (!seen_names.insert(entry.name).second) {
                throw std::runtime_error("duplicate server name '" + entry.name + "'");
            }
            config.entries.push_back(std::move(entry));
        } catch (const std::exception& error) {
            config.errors.push_back(EntryLabel(doc.at(i), i) + ": " + error.what());
        }
    }
    return config;
}

McpsConfig LoadMcpsConfig(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) return McpsConfig{};  // absence is the normal "no MCP" state, not an error
    nlohmann::json doc;
    try {
        in >> doc;
    } catch (const nlohmann::json::exception& error) {
        throw std::runtime_error("mcps.json: invalid JSON: " + std::string(error.what()));
    }
    return ParseMcpsConfig(doc);
}

}  // namespace minimal::config
