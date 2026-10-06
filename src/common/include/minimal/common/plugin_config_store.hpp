#pragma once

// Editing one plugin's config file (plugins.d/<name>.json) against its
// schema (config::SchemaRecording's shape). Pure JSON work plus the file's
// read/write -- shared by the `minimal plugin` CLI and the server's config
// endpoint, so both edit the same way. Paths are dotted ("search.api_url").

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace minimal::common {

// The value at `path`, or nullptr.
const nlohmann::json* ConfigGet(const nlohmann::json& config, std::string_view path);
// Sets `path`, creating intermediate objects. Throws std::invalid_argument
// when a prefix of `path` holds a non-object value.
void ConfigSet(nlohmann::json& config, std::string_view path, nlohmann::json value);
// Removes `path`, pruning parents it leaves empty. False when absent.
bool ConfigUnset(nlohmann::json& config, std::string_view path);

// The schema's field for `path`, or nullptr.
const nlohmann::json* SchemaField(const nlohmann::json& schema, std::string_view path);

// Command-line text -> a value of the field's type: integers and numbers
// parse as such (range-checked), booleans as true/false, an enum must be
// one of its choices, a string is taken verbatim, a "json" or list/map field
// parses as JSON. Throws std::invalid_argument with a printable message.
nlohmann::json ParseFieldValue(const nlohmann::json& field, std::string_view text);

// One row per schema field: what a user who wrote `config` effectively has.
struct EffectiveField {
    std::string path;
    nlohmann::json value;  // the configured value, else the default (null when neither)
    bool overridden = false;
    const nlohmann::json* field = nullptr;  // into the schema
};
std::vector<EffectiveField> EffectiveConfig(const nlohmann::json& schema,
                                            const nlohmann::json& config);

// Schema paths whose effective value differs between `before` and `after`,
// and whether every one of them is "apply": "live". A path the schema does
// not know counts as not live.
struct ConfigChange {
    std::vector<std::string> paths;
    bool all_live = true;
};
ConfigChange DiffConfig(const nlohmann::json& schema, const nlohmann::json& before,
                        const nlohmann::json& after);

// A value as shown to a person: a secret field's literal is masked ("sk-...
// " -> "****"); a "secret:"/"env:" reference is shown as written.
std::string DisplayValue(const nlohmann::json& field, const nlohmann::json& value);

// The file: {} when it does not exist; throws config::ConfigError when it
// cannot be read or is not a JSON object.
nlohmann::json ReadPluginConfigFile(const std::filesystem::path& path, const std::string& label);
// Atomic, creating the directory. An empty object removes the file instead
// (nothing configured is the same as no file). "" on success, else the error.
std::string WritePluginConfigFile(const std::filesystem::path& path, const nlohmann::json& config);

}  // namespace minimal::common
