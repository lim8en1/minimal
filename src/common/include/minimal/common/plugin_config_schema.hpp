#pragma once

// A plugin's config schema and dry-run validation, from its own parser. An
// SDK header: one MINIMAL_PLUGIN_CONFIG_SCHEMA line exports both optional
// ABI symbols (plugin_abi.h) from the function the plugin already uses to
// read its plugins.d file.
//
//   MINIMAL_PLUGIN_CONFIG_SCHEMA(
//       minimal::common::PluginConfigLabel(minimal::common::kPluginConfigDirName, "tools-bash"),
//       [](const nlohmann::json& config) { (void)ParseBashSettings(config); })
//
// The parser must be side-effect free for both: it runs at describe time
// with no registration context (the CLI, the config view), and against
// candidate values the user has not saved yet.

#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "minimal/common/config_reader.hpp"

namespace minimal::config {

// `parse` run against {} in describe mode: SchemaRecording::Schema()'s shape.
template <typename Parse>
nlohmann::json DescribeConfig(const std::string& file_label, Parse&& parse) {
    SchemaRecording recording(file_label);
    std::forward<Parse>(parse)(nlohmann::json::object());
    return recording.Schema();
}

// `parse` run against `config`: "" when it accepts it, else its message.
template <typename Parse>
std::string ValidateConfig(Parse&& parse, const nlohmann::json& config) {
    try {
        std::forward<Parse>(parse)(config);
        return {};
    } catch (const std::exception& error) {
        return error.what();
    }
}

}  // namespace minimal::config

// Defines minimal_plugin_config_schema and minimal_plugin_validate_config
// over `parse` (callable as parse(const nlohmann::json&)). `label` is the
// plugin's own file label, the same one its Reader uses. Use at namespace
// scope, outside any extern "C" block.
#define MINIMAL_PLUGIN_CONFIG_SCHEMA(label, parse)                                       \
    extern "C" void minimal_plugin_config_schema(nlohmann::json& out) {                  \
        out = ::minimal::config::DescribeConfig((label), (parse));                       \
    }                                                                                    \
    extern "C" void minimal_plugin_validate_config(const nlohmann::json& config,         \
                                                   std::string& error) {                 \
        error = ::minimal::config::ValidateConfig((parse), config);                      \
    }
