#include "minimal/mcp/mcps_config.hpp"

#include <map>
#include <optional>
#include <unordered_set>

#include "minimal/common/json_config_loader.hpp"
#include "minimal/common/config_reader.hpp"

namespace minimal::config {

namespace {

// The entry's name when it has a usable one (diagnostics read like the other
// loaders'), else its position -- for an entry whose name is the problem.
std::string EntryPath(const nlohmann::json& entry, std::size_t index) {
    if (entry.is_object() && entry.contains("name") && entry.at("name").is_string() &&
        !entry.at("name").get<std::string>().empty()) {
        return entry.at("name").get<std::string>();
    }
    return "#" + std::to_string(index);
}

std::unordered_map<std::string, std::string> ToUnordered(
    std::optional<std::map<std::string, std::string>> map) {
    if (!map) return {};
    return {map->begin(), map->end()};
}

McpServerEntry ParseEntry(Reader& entry) {
    McpServerEntry result;
    result.name = entry.RequireString("name");
    const std::string transport = *entry.MaybeEnum("transport", {"stdio", "http"});
    if (transport == "stdio") {
        result.transport = McpTransport::Stdio;
        result.command = entry.RequireString("command");
        result.args = entry.StringList("args", /*allow_empty_entries=*/true);
        result.env = ToUnordered(entry.MaybeStringMap("env"));
    } else {
        result.transport = McpTransport::Http;
        result.url = entry.RequireString("url");
        result.headers = ToUnordered(entry.MaybeStringMap("headers"));
    }
    result.enabled = entry.Bool("enabled", result.enabled);
    entry.Finish();
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
    if (!doc.is_array()) throw ConfigError("mcps.json: expected a top-level array");

    McpsConfig config;
    std::unordered_set<std::string> seen_names;
    for (std::size_t i = 0; i < doc.size(); ++i) {
        // A bad entry degrades to a recorded reason; the good ones survive.
        try {
            const auto& node = doc.at(i);
            if (!node.is_object()) {
                throw ConfigError("mcps.json: " + EntryPath(node, i) + ": must be an object");
            }
            Reader reader(node, "mcps.json", EntryPath(node, i));
            if (!reader.Has("transport")) reader.Fail("transport", "is required");
            McpServerEntry entry = ParseEntry(reader);
            if (!seen_names.insert(entry.name).second) {
                reader.Fail("name", "duplicate server name '" + entry.name + "'");
            }
            config.entries.push_back(std::move(entry));
        } catch (const ConfigError& error) {
            config.errors.push_back(error.what());
        }
    }
    return config;
}

McpsConfig LoadMcpsConfig(const std::filesystem::path& path) {
    // Absence is the normal "no MCP" state, not an error.
    return LoadJsonConfig<McpsConfig>(path, "mcps.json", AbsencePolicy::kDefaultOnMissing,
                                      EmptyFilePolicy::kTreatAsMalformed, ParseMcpsConfig);
}

}  // namespace minimal::config
