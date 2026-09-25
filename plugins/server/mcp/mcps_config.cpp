#include "minimal/mcp/mcps_config.hpp"

#include <stdexcept>
#include <unordered_set>

#include "minimal/config/json_config_loader.hpp"
#include "minimal/config/parse_util.hpp"

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

// No index parameter: the caller catches and labels every failure with
// EntryLabel(entry, i), so this throws bare reasons and never has to know where
// it sits in the array.
McpServerEntry ParseEntry(const nlohmann::json& entry) {
    if (!entry.is_object()) throw std::runtime_error("must be an object");

    McpServerEntry result;
    // contains + type checks, not entry.at(): .at() throws a raw
    // json.exception.out_of_range ("key 'name' not found") which the blanket
    // catch below would hand to the user verbatim, while every neighbouring
    // field has a hand-written message. Same for "transport".
    if (!entry.contains("name") || !entry.at("name").is_string()) {
        throw std::runtime_error("name is required and must be a string");
    }
    result.name = entry.at("name").get<std::string>();
    if (result.name.empty()) throw std::runtime_error("name must be a non-empty string");

    if (!entry.contains("transport") || !entry.at("transport").is_string()) {
        throw std::runtime_error("transport is required and must be \"stdio\" or \"http\"");
    }
    const std::string transport = entry.at("transport").get<std::string>();
    if (transport == "stdio") {
        result.transport = McpTransport::Stdio;
        // contains + type check, not entry.at(): same reasoning as
        // "name"/"transport" above -- a missing "command" must not surface a
        // raw json.exception.out_of_range through the blanket catch below.
        if (!entry.contains("command") || !entry.at("command").is_string()) {
            throw std::runtime_error("stdio entry needs a non-empty command");
        }
        result.command = entry.at("command").get<std::string>();
        if (result.command.empty()) throw std::runtime_error("stdio entry needs a non-empty command");
        if (entry.contains("args")) {
            // is_array() first: nlohmann's range-for over a non-array value
            // still "works" (a scalar iterates once over itself; an object
            // iterates its VALUES in key-sorted order), so without this check
            // a typo'd object or string silently became a one-element or
            // reordered args list instead of the intended rejection.
            if (!entry.at("args").is_array()) {
                throw std::runtime_error("args must be an array of strings");
            }
            for (const auto& arg : entry.at("args")) {
                if (!arg.is_string()) throw std::runtime_error("args must be an array of strings");
                result.args.push_back(arg.get<std::string>());
            }
        }
        if (entry.contains("env")) result.env = ParseStringMap(entry.at("env"), "env");
    } else if (transport == "http") {
        result.transport = McpTransport::Http;
        if (!entry.contains("url") || !entry.at("url").is_string()) {
            throw std::runtime_error("http entry needs a non-empty url");
        }
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
            McpServerEntry entry = ParseEntry(doc.at(i));
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
    // Absence is the normal "no MCP" state, not an error.
    return LoadJsonConfig<McpsConfig>(path, "mcps", AbsencePolicy::kDefaultOnMissing,
                                      EmptyFilePolicy::kTreatAsMalformed, ParseMcpsConfig);
}

}  // namespace minimal::config
