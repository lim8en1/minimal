#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

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
    // S4: selects a CompactionStrategyRegistry entry by name. "summarize" is
    // core's own built-in (registered in main.cpp, not a plugin, so this
    // never resolves to nothing on a bundled-only install). A name naming no
    // registered strategy is a startup sanity-check warning, not a crash --
    // the session falls back to the built-in.
    std::string compaction_strategy = "summarize";
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
    // tools/list pagination cap for the http transport. A server that never
    // stops returning nextCursor pages would otherwise wedge the connection's
    // worker thread (each page is individually within request_timeout). The
    // stdio transport answers from a single request and needs no cap.
    std::int64_t max_list_pages = 100;
    // Floor between restart ATTEMPTS for one server after a failed one (not a
    // cap on liveness checks -- a reused, healthy connection is unaffected).
    // Without it, a subagent spawn loop against a down server pays a full
    // spawn+initialize+discovery attempt per spawn, at spawn rate.
    std::int64_t restart_cooldown_ms = 30000;
};

struct ToolsSettings {
    nlohmann::json read = nlohmann::json::object();
    nlohmann::json edit = nlohmann::json::object();
    nlohmann::json bash = nlohmann::json::object();
    McpSettings mcp;
    // How many tool calls from one assistant turn may execute concurrently
    // (parallel dispatch -- see ARCHITECTURE.md "Tool dispatch"). 1 reproduces
    // the sequential behavior exactly. This is a WIDTH, not a thread-pool size:
    // the per-session pool is built from it, so 1 allocates no extra threads.
    int max_parallel_tool_calls = 4;
    // THE global ceiling on one tool call's serialized input. Every other
    // model-authored allocation has a bound; the input itself -- copied into
    // lifecycle events, permission requests and hook payloads -- was bounded
    // only by the model's output budget. Over the cap is an error result the
    // model adapts to, like every other refusal. 0 disables.
    std::int64_t max_input_bytes = 10 * 1024 * 1024;
    // THE global ceiling on one tool call's RESULT, checked once after
    // execution regardless of which tool produced it. A core tool like bash
    // caps its own output (bash.max_output_bytes), but that cap is the
    // plugin's own choice, not a harness guarantee -- an MCP tool or a
    // third-party plugin tool has no ceiling at all short of this one, so an
    // oversized response would otherwise ride straight into the model
    // request, into SQLite, and into every later turn's prefix until
    // compaction. Truncated over the cap, with a visible marker, never an
    // error result -- a large result is not a refusal. 0 disables.
    std::int64_t max_result_bytes = 10 * 1024 * 1024;
};

struct ToolsViewSettings {
    std::int64_t stdout_buffer_lines = 100;
};

// Project context files (AGENTS.md, CLAUDE.md): read once per session from the
// session's project_root -- at creation, resume, and subagent spawn -- and
// appended to the system prompt. This is a SESSION property, not a preset one:
// a mid-session /agent re-resolves the preset and must not drop the project's
// instructions (see AgentLoop::BuildChatRequest).
//
// `files` is tried in order WITHIN each directory. With search_ancestors the
// search also walks upward from project_root, nearest directory first -- so a
// CLAUDE.md sitting IN the project beats an AGENTS.md three levels up, because
// closer instructions are more specific. Walking stops at the filesystem root,
// which makes a ~/AGENTS.md a global fallback; that is a feature, not an
// accident to fence off. max_bytes == 0 means unlimited.
struct ProjectContextSettings {
    bool enabled = true;
    std::vector<std::string> files = {"AGENTS.md", "CLAUDE.md"};
    std::int64_t max_bytes = 0;
    bool search_ancestors = false;
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
    // When an out-of-band message (a background task's result, a non-blocking
    // subagent's completion) lands on an IDLE session, run a turn for it
    // immediately instead of waiting for the next user turn. Off by default:
    // a wake is a real provider call the user did not ask for. Per-session,
    // resolved at creation; a session never changes its answer mid-life.
    bool background_wake = false;
    MemorySettings memory;
    ToolsSettings tools;
    ToolsViewSettings tools_view;
    ProjectContextSettings project_context;
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
