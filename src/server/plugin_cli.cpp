#include "plugin_cli.hpp"

#include <algorithm>
#include <cstdio>
#include <optional>

#include <nlohmann/json.hpp>

#include "minimal/common/config_reader.hpp"
#include "minimal/common/plugin_config_label.hpp"
#include "minimal/common/plugin_config_store.hpp"
#include "minimal/common/read_file.hpp"
#include "minimal/plugins/plugin_discovery.hpp"

namespace minimal::server {
namespace {

using nlohmann::json;

constexpr const char* kUsage =
    "usage: minimal plugin [--json]                      list plugins\n"
    "       minimal plugin <name> [--json]               show a plugin's config\n"
    "       minimal plugin get <name> <key> [--json]     one value\n"
    "       minimal plugin set <name> <key> <value>      set a value\n"
    "       minimal plugin unset <name> <key>            back to the default\n";

struct Located {
    std::string name;  // the module's stem: plugins.d/<name>.json
    std::filesystem::path file;
    bool enabled = false;
};

// Every plugin module, as plugins.json records it -- read, never rewritten.
std::vector<Located> Discover(const std::filesystem::path& config_dir) {
    plugins::PluginConfig list =
        plugins::ParsePluginConfig(common::ReadFileOrEmpty(config_dir / "plugins.json"));
    std::vector<Located> out;
    for (const auto& candidate :
         plugins::DiscoverPlugins(plugins::BundledPluginDir(), config_dir / "plugins", list)) {
        out.push_back({candidate.file.stem().string(), candidate.file, candidate.enabled});
    }
    return out;
}

std::optional<Located> Find(const std::vector<Located>& all, const std::string& name) {
    for (const auto& plugin : all) {
        if (plugin.name == name) return plugin;
    }
    return std::nullopt;
}

// One plugin opened for editing: its schema, validator, file and contents.
struct Target {
    Located plugin;
    plugins::InspectedPlugin inspected;
    json schema;
    std::filesystem::path path;
    std::string label;
    json config;
};

// Prints why and returns nullopt when the plugin is unknown, cannot be
// opened, has no config, or its file cannot be read.
std::optional<Target> Open(const std::filesystem::path& config_dir, const std::string& name) {
    const auto all = Discover(config_dir);
    const auto plugin = Find(all, name);
    if (!plugin) {
        std::string known;
        for (const auto& p : all) known += (known.empty() ? "" : ", ") + p.name;
        std::fprintf(stderr, "minimal: no plugin '%s' (known: %s)\n", name.c_str(), known.c_str());
        return std::nullopt;
    }
    Target target{*plugin, plugins::InspectPluginConfig(plugin->file)};
    if (!target.inspected.error.empty()) {
        std::fprintf(stderr, "minimal: %s: %s\n", name.c_str(), target.inspected.error.c_str());
        return std::nullopt;
    }
    if (target.inspected.config.schema == nullptr) {
        std::fprintf(stderr, "minimal: plugin '%s' has no configuration\n", name.c_str());
        return std::nullopt;
    }
    try {
        target.inspected.config.schema(target.schema);
        target.label = common::PluginConfigLabel(common::kPluginConfigDirName, name);
        target.path = config_dir / common::kPluginConfigDirName / (name + ".json");
        target.config = common::ReadPluginConfigFile(target.path, target.label);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "minimal: %s\n", error.what());
        return std::nullopt;
    }
    return target;
}

std::string KnownKeys(const json& schema) {
    std::string out;
    for (const auto& field : schema.at("fields")) {
        out += (out.empty() ? "" : ", ") + field.value("path", "");
    }
    return out;
}

const json* RequireField(const Target& target, const std::string& key) {
    const json* field = common::SchemaField(target.schema, key);
    if (field == nullptr) {
        std::fprintf(stderr, "minimal: %s: unknown key '%s' (known: %s)\n", target.label.c_str(),
                     key.c_str(), KnownKeys(target.schema).c_str());
    }
    return field;
}

int List(const std::filesystem::path& config_dir, bool as_json) {
    json rows = json::array();
    std::size_t width = 0;
    for (const auto& plugin : Discover(config_dir)) {
        const auto path = config_dir / common::kPluginConfigDirName / (plugin.name + ".json");
        // Opened for its schema symbol only: does it take configuration at all?
        const auto inspected = plugins::InspectPluginConfig(plugin.file);
        json row = {{"name", plugin.name},
                    {"enabled", plugin.enabled},
                    {"configurable", inspected.config.schema != nullptr},
                    {"set", 0}};
        if (!inspected.error.empty()) row["error"] = inspected.error;
        try {
            const json config = common::ReadPluginConfigFile(
                path, common::PluginConfigLabel(common::kPluginConfigDirName, plugin.name));
            // flatten() of {} is one empty-key entry, not zero.
            row["set"] = config.empty() ? 0 : config.flatten().size();
        } catch (const std::exception& error) {
            row["error"] = error.what();
        }
        width = std::max(width, plugin.name.size());
        rows.push_back(std::move(row));
    }
    if (as_json) {
        std::printf("%s\n", rows.dump(2).c_str());
        return 0;
    }
    for (const auto& row : rows) {
        std::string detail;
        if (row.contains("error")) {
            detail = row.at("error").get<std::string>();
        } else if (row.at("set").get<std::size_t>() > 0) {
            detail = std::to_string(row.at("set").get<std::size_t>()) + " set in " +
                     common::kPluginConfigDirName + "/" + row.at("name").get<std::string>() +
                     ".json";
        } else if (row.at("configurable").get<bool>()) {
            detail = "defaults";
        } else {
            detail = "-";  // takes no configuration
        }
        std::printf("%-*s  %-8s  %s\n", static_cast<int>(width),
                    row.at("name").get<std::string>().c_str(),
                    row.at("enabled").get<bool>() ? "enabled" : "disabled", detail.c_str());
    }
    return 0;
}

int Show(const Target& target, bool as_json) {
    const auto rows = common::EffectiveConfig(target.schema, target.config);
    if (as_json) {
        json values = json::object();
        json unused = json::object();
        for (const auto& row : rows) {
            values[row.path] = row.value;
            if (row.unused) unused[row.path] = *row.unused;
        }
        std::printf("%s\n", json{{"file", target.label},
                                 {"unused", unused},
                                 {"enabled", target.plugin.enabled},
                                 {"schema", target.schema},
                                 {"config", target.config},
                                 {"effective", values}}
                                .dump(2)
                                .c_str());
        return 0;
    }
    std::size_t path_width = 0;
    std::size_t value_width = 0;
    std::vector<std::string> shown;
    for (const auto& row : rows) {
        shown.push_back(common::DisplayValue(*row.field, row.value));
        path_width = std::max(path_width, row.path.size());
        value_width = std::max(value_width, std::min<std::size_t>(shown.back().size(), 40));
    }
    std::printf("%s%s\n", target.label.c_str(), target.plugin.enabled ? "" : "  (plugin disabled)");
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& row = rows[i];
        // An unused field (its "when" unmet) says why instead of its apply
        // policy: what it would take to matter is the useful fact.
        const std::string apply =
            row.unused ? "-" : row.field->value("apply", "restart");
        const std::string doc =
            row.unused ? "(not used: " + *row.unused + ") " + row.field->value("doc", "")
                       : row.field->value("doc", "");
        std::printf("  %c %-*s  %-*s  %-7s  %s\n", row.overridden ? '*' : ' ',
                    static_cast<int>(path_width), row.path.c_str(), static_cast<int>(value_width),
                    shown[i].c_str(), apply.c_str(), doc.c_str());
    }
    std::printf("\n  * set in the file; the rest are defaults\n");
    return 0;
}

int Get(const Target& target, const std::string& key, bool as_json) {
    const json* field = RequireField(target, key);
    if (field == nullptr) return 1;
    const json* value = common::ConfigGet(target.config, key);
    const json shown = value != nullptr ? *value : field->value("default", json());
    if (as_json) {
        std::printf("%s\n", shown.dump().c_str());
    } else if (shown.is_string()) {
        std::printf("%s\n", shown.get<std::string>().c_str());  // shell-friendly: unquoted
    } else {
        std::printf("%s\n", shown.dump().c_str());
    }
    return 0;
}

// Validates `next` with the plugin's own parser, writes it, and says how the
// change applies.
int Save(const Target& target, const json& next, const std::string& what) {
    std::string error;
    target.inspected.config.validate(next, error);
    if (!error.empty()) {
        std::fprintf(stderr, "minimal: %s\n", error.c_str());
        return 1;
    }
    const auto change = common::DiffConfig(target.schema, target.config, next);
    if (const std::string failed = common::WritePluginConfigFile(target.path, next);
        !failed.empty()) {
        std::fprintf(stderr, "minimal: %s\n", failed.c_str());
        return 1;
    }
    if (change.paths.empty() && !change.unused.empty()) {
        const json* field = common::SchemaField(target.schema, change.unused.front());
        const auto reason = field != nullptr ? common::UnusedReason(target.schema, *field, next)
                                             : std::nullopt;
        std::printf("%s: %s (saved; not used while %s)\n", target.label.c_str(), what.c_str(),
                    reason ? reason->c_str() : "its condition is unmet");
    } else if (change.paths.empty()) {
        std::printf("%s: %s (no change)\n", target.label.c_str(), what.c_str());
    } else if (!target.plugin.enabled) {
        std::printf("%s: %s (the plugin is disabled; applies when it is enabled)\n",
                    target.label.c_str(), what.c_str());
    } else if (change.all_live) {
        std::printf("%s: %s (live: a running server takes it without a restart)\n",
                    target.label.c_str(), what.c_str());
    } else {
        std::printf("%s: %s (restart: takes effect when the server restarts)\n",
                    target.label.c_str(), what.c_str());
    }
    return 0;
}

int Set(const Target& target, const std::string& key, const std::string& text) {
    const json* field = RequireField(target, key);
    if (field == nullptr) return 1;
    json next = target.config;
    try {
        common::ConfigSet(next, key, common::ParseFieldValue(*field, text));
    } catch (const std::exception& error) {
        std::fprintf(stderr, "minimal: %s: %s\n", target.label.c_str(), error.what());
        return 1;
    }
    return Save(target, next, key + " = " + common::DisplayValue(*field, *common::ConfigGet(next, key)));
}

int Unset(const Target& target, const std::string& key) {
    const json* field = RequireField(target, key);
    if (field == nullptr) return 1;
    json next = target.config;
    if (!common::ConfigUnset(next, key)) {
        std::printf("%s: %s is not set (default %s)\n", target.label.c_str(), key.c_str(),
                    common::DisplayValue(*field, field->value("default", json())).c_str());
        return 0;
    }
    return Save(target, next, key + " back to its default");
}

}  // namespace

int RunPluginCli(const std::vector<std::string>& raw_args, const std::filesystem::path& config_dir) {
    std::vector<std::string> args;
    bool as_json = false;
    for (const auto& arg : raw_args) {
        if (arg == "--json") {
            as_json = true;
        } else if (arg == "--help" || arg == "-h") {
            std::printf("%s", kUsage);
            return 0;
        } else {
            args.push_back(arg);
        }
    }
    const auto usage = [] {
        std::fprintf(stderr, "%s", kUsage);
        return 2;
    };

    try {
        if (args.empty()) return List(config_dir, as_json);
        const std::string& verb = args[0];
        if (verb == "get" || verb == "set" || verb == "unset") {
            const std::size_t needed = verb == "set" ? 4 : 3;
            if (args.size() != needed) return usage();
            const auto target = Open(config_dir, args[1]);
            if (!target) return 1;
            if (verb == "get") return Get(*target, args[2], as_json);
            if (verb == "set") return Set(*target, args[2], args[3]);
            return Unset(*target, args[2]);
        }
        if (args.size() != 1) return usage();
        const auto target = Open(config_dir, verb);
        return target ? Show(*target, as_json) : 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "minimal: %s\n", error.what());
        return 1;
    }
}

}  // namespace minimal::server
