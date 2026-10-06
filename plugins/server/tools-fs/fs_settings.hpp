#pragma once

#include <cstdint>

#include <nlohmann/json.hpp>

namespace minimal::tools_fs {

// This plugin's own config, the "config" object on its plugins.json entry,
// with one sub-object per tool: {"read": {...}, "edit": {...}}. Core keeps it
// as raw JSON precisely so each plugin owns its own schema -- this is that
// schema, and nothing outside tools-fs needs to know it.
struct FsSettings {
    int read_default_limit_lines = 2000;
    int read_max_line_length = 2000;
    // THE hard ceiling on any read window, whatever the model asked for. The
    // memory bound "proportional to limit, not file size" is hollow when limit
    // is model-controlled: `{"limit": 0}` used to mean the WHOLE FILE, and a
    // 10 GB log was an OOM on the session worker. limit == 0 now means this
    // ceiling too, and clamping is disclosed in the result footer.
    int read_max_lines = 2000;
    // Bounds worst-case memory for the mutating edit commands, which (unlike
    // read) must hold the whole file to rewrite it. Past this, the model is told
    // to use a different approach rather than the server quietly allocating a
    // gigabyte.
    std::int64_t edit_max_editable_file_bytes = 5 * 1024 * 1024;
};

// Every field optional; a missing field keeps the struct's default. A present
// field of the wrong type or out of range, or an unknown field, throws
// config::ConfigError naming it -- the plugin then fails to load, loudly.
FsSettings ParseFsSettings(const nlohmann::json& config);

}  // namespace minimal::tools_fs
