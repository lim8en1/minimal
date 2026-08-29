#pragma once

#include <cstdint>

#include <nlohmann/json.hpp>

namespace minimal::tools_fs {

// This plugin's slice of settings.json's `tools` object. Core keeps that object
// as raw JSON precisely so each plugin owns its own schema -- this is that
// schema, and nothing outside tools-fs needs to know it.
struct FsSettings {
    int read_default_limit_lines = 2000;
    int read_max_line_length = 2000;
    // Bounds worst-case memory for the mutating edit commands, which (unlike
    // read) must hold the whole file to rewrite it. Past this, the model is told
    // to use a different approach rather than the server quietly allocating a
    // gigabyte.
    std::int64_t edit_max_editable_file_bytes = 5 * 1024 * 1024;
};

// Every field optional; missing values keep the struct's defaults, so an absent
// or partial `tools` object is normal rather than an error.
FsSettings ParseFsSettings(const nlohmann::json& tools_settings);

}  // namespace minimal::tools_fs
