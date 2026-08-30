#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

namespace minimal::config {

struct MemorySettings {
    bool auto_compact = true;
    std::int64_t context_compaction_trigger_tokens = 0;
    std::int64_t compaction_keep_recent_tokens = 0;
    std::int64_t scrollback_page_size = 0;
};

// Kept as raw pass-through JSON: each plugin owns the schema of its own slice
// (tools-fs parses `read`/`edit`, tools-bash parses `bash`), so core never has
// to match or break their shapes.
struct ToolsSettings {
    nlohmann::json read = nlohmann::json::object();
    nlohmann::json edit = nlohmann::json::object();
    nlohmann::json bash = nlohmann::json::object();
};

struct ToolsViewSettings {
    std::int64_t stdout_buffer_lines = 100;
};

struct LoggingSettings {
    std::string level = "info";
    std::string path;
    std::int64_t max_size_mb = 10;
    std::int64_t max_files = 5;
};

struct SettingsConfig {
    std::string default_agent_preset;
    // "<provider>/<model>". Was default_provider, which named a connection back
    // when a connection implied exactly one model.
    std::string default_model;
    std::string default_theme;
    std::int64_t max_concurrent_subagents = 4;
    MemorySettings memory;
    ToolsSettings tools;
    ToolsViewSettings tools_view;
    std::string session_db_path;
    LoggingSettings logging;
};

// Pure. Every field is optional; missing fields take the struct's own default.
// This loader does no cross-field validation (e.g. an empty default_model is
// only an error later, if a preset actually resolves `provider: "default"`) --
// that belongs to the future sanity_check, not here.
SettingsConfig ParseSettingsConfig(const nlohmann::json& doc);

// Thin: reads the file and parses it. Throws if the file is missing or isn't
// valid JSON.
SettingsConfig LoadSettingsConfig(const std::filesystem::path& path);

}  // namespace minimal::config
