#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

namespace minimal::config {

// When automatic context compaction fires. Exactly one strategy; the loader
// rejects anything else. Absent means "never trigger".
// - FixedTokens:      trigger when context occupancy crosses `tokens`.
// - PercentOfWindow:  trigger when occupancy crosses `percent`% of the active
//                     provider entry's context_window_tokens.
// - WindowBuffer:     trigger when occupancy comes within `buffer_tokens` of
//                     the window (trigger = window - buffer).
// The window-relative strategies need a window to be relative to: a provider
// entry without context_window_tokens means the trigger never fires (the
// agent loop resolves this and says so; see agent/compaction_trigger).
struct CompactionTriggerSettings {
    enum class Kind { Never, FixedTokens, PercentOfWindow, WindowBuffer };
    Kind kind = Kind::Never;
    std::int64_t tokens = 0;         // valid iff kind == FixedTokens
    int percent = 0;                 // valid iff kind == PercentOfWindow, 1..100
    std::int64_t buffer_tokens = 0;  // valid iff kind == WindowBuffer
};

struct MemorySettings {
    bool auto_compact = true;
    CompactionTriggerSettings context_compaction_trigger;
    std::int64_t compaction_keep_recent_tokens = 0;
    std::int64_t scrollback_page_size = 0;
};

// Kept as raw pass-through JSON: each plugin owns the schema of its own slice
// (tools-fs parses `read`/`edit`, tools-bash parses `bash`), so core never has
// to match or break their shapes.
struct McpSettings {
    // Spawn/handshake deadline and per-call deadline. The MCP client wrapper
    // enforces these itself: cpp-mcp's client blocks internally (up to a fixed
    // 60 s) with no deadline parameter, and a dead or hanging MCP server must
    // degrade to a tool error, never a hung session worker.
    std::int64_t connect_timeout_ms = 10000;
    std::int64_t request_timeout_ms = 60000;
};

struct ToolsSettings {
    nlohmann::json read = nlohmann::json::object();
    nlohmann::json edit = nlohmann::json::object();
    nlohmann::json bash = nlohmann::json::object();
    McpSettings mcp;
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
