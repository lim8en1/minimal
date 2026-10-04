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
    // Share of the working history (by estimated tokens, oldest first) that a
    // compaction REPLACES with the summary; the rest stays verbatim after it.
    // The summarizer itself always reads the whole history. 1..100; 100
    // replaces everything but the newest message (always kept whole, and the
    // cut never splits a tool call from its result). Ignored when
    // compaction_keep_recent_tokens is set (an absolute budget wins).
    int compaction_replace_percent = 80;
    std::int64_t scrollback_page_size = 0;
    // S4: selects a CompactionStrategyRegistry entry by name. "summarize"
    // (free-text summary head) and "structured" (a strict record_summary
    // tool call parsed into typed sections) are core's own built-ins
    // (registered in main.cpp, not plugins, so this never resolves to nothing
    // on a bundled-only install); "structured" is the default. A name naming
    // no registered strategy is a startup sanity-check warning, not a crash --
    // the session falls back to the built-in. An agent preset's
    // `compaction_strategy:` frontmatter overrides this per agent.
    std::string compaction_strategy = "structured";
    // Some OpenAI-compatible proxies report an all-zero Usage; without a
    // fallback the compaction trigger never arms and the status bar reads
    // 0/max forever. When set (the default), a zero-usage turn seeds
    // occupancy from the request's bytes/4 estimate and flags the turn's
    // wire usage as estimated. Never feeds cost pricing -- an estimated
    // token count is not a reported one.
    bool estimate_zero_usage = true;
};

// Kept as raw pass-through JSON: each plugin owns the schema of its own slice
// (tools-fs parses its own tool config, tools-bash parses `bash`), so core
// never has to match or break their shapes.
struct ToolsSettings {
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
    // Ceiling on one result's client-only display payload (serialized JSON).
    // Over it the payload is DROPPED, never truncated -- a cut diff misleads --
    // and the client falls back to the text. 0 disables.
    std::int64_t max_display_bytes = 128 * 1024;
    // The full parsed "tools" JSON object, verbatim -- alongside the typed
    // fields above -- so a plugin's own top-level key under "tools" that
    // ParseTools does not recognize still reaches PluginLoader intact. This
    // is what main.cpp forwards as context.tools_settings: core imposes no
    // schema on that slice, and the typed fields above exist for core's own
    // consumers, not to replace this passthrough.
    nlohmann::json raw = nlohmann::json::object();
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
// accident to fence off. max_bytes == 0 means unlimited. A symlinked file
// that resolves outside the directory it was found in is ignored.
struct ProjectContextSettings {
    bool enabled = true;
    std::vector<std::string> files = {"AGENTS.md", "CLAUDE.md"};
    std::int64_t max_bytes = 262144;
    bool search_ancestors = false;
};

// Whether the project's own agents/, skills/ and commands/ directories (found by
// walking up from the session's project root to its git root) are loaded. They
// are content a cloned repository ships, so by default they load only for
// directories the operator has trusted (`/trust-directory`). `project_context`
// (AGENTS.md / CLAUDE.md) has its own switch and is not governed by this.
enum class ProjectLocalMode {
    kTrusted,  // load for trusted directories only (the default)
    kAll,      // load for every project (the behaviour before the trust gate)
    kNone,     // never load project-local content
};

struct ProjectLocalSettings {
    ProjectLocalMode mode = ProjectLocalMode::kTrusted;
};

// Plugin context contributors (agent/context_contributor.hpp).
struct ContextContributorSettings {
    // Plugin names whose contributed sections are skipped -- the operator's off
    // switch that does not require disabling the whole plugin.
    std::vector<std::string> disabled;
    // Per-section body cap in bytes. 0 = uncapped.
    std::int64_t max_bytes_per_section = 16384;
};

struct LoggingSettings {
    std::string level = "info";
    std::string path;
    std::int64_t max_size_mb = 10;
    std::int64_t max_files = 5;
};

// Session lifetime policy (S5). Every timeout is a settings.json key with a
// documented default -- none of these is hardcoded at the use site -- and
// 0 means "never" wherever a 0 makes sense, so an operator can opt into the
// extreme policy rather than discover it is the only one.
struct SessionSettings {
    // How long a fully QUiescent detached session (no turn in flight, no live
    // background tasks, no subagent children, no subscribers) lives before the
    // reaper terminates it. Collecting a quiescent session is lossless: the
    // transcript stays in SQLite and re-attach becomes resume. 0 = never reap
    // (sessions live until explicitly terminated or deleted).
    std::int64_t detached_idle_ttl_seconds = 1800;
    // An unanswered permission/confirmation prompt is denied after this long.
    // 0 (the default) waits forever: a human may simply be away, and
    // auto-denying their work is worse than waiting (ARCHITECTURE.md).
    std::int64_t unattended_prompt_timeout_seconds = 0;
    // The never-attached guard: a session no client attaches to is terminated
    // after this grace period. Purely a safety net against a non-conforming
    // client; surfacing it here replaced a hardcoded constructor default.
    std::int64_t initial_attach_guard_seconds = 10;
};

// Server process lifetime. With detach, the server legitimately outlives its
// clients while sessions are still live; this governs the tail.
struct ServerSettings {
    // Once there are zero attached clients AND zero live sessions, exit after
    // this linger. 0 = never exit (true daemon deployment).
    std::int64_t idle_shutdown_seconds = 5;
    // Wall-clock budget for the post-listen background warm-up of providers.json
    // "discover": true entries, so the /model picker lists discovered models on a
    // freshly started server without a manual refresh. The fetch runs on its own
    // thread (startup never waits on it); this bounds only that thread. 0 =
    // disabled (static models only until ctrl+r or first resolve).
    std::int64_t discovery_warmup_timeout_seconds = 15;
};

struct SettingsConfig {
    std::string default_agent_preset;
    // "<provider>/<model>". Was default_provider, which named a connection back
    // when a connection implied exactly one model.
    std::string default_model;
    // opencode-compat: an alternate <provider>/<model> for SUMMARIZATION calls
    // (compaction). Empty = compact on the session's own model. Shape-checked
    // by the sanity check; a reference providers.json cannot resolve warns and
    // behaves as empty.
    std::string small_model;
    // "<provider>/<model>" for spawned subagents. Empty = a child inherits
    // its parent's model (unless its preset declares one or something more
    // specific overrides it). Shape-checked by the sanity check; a reference
    // providers.json cannot resolve warns and behaves as empty.
    std::string default_subagent_model;
    std::string default_theme;
    std::int64_t max_concurrent_subagents = 4;
    // When an out-of-band message (a background task's result, a non-blocking
    // subagent's completion) lands on an IDLE session, run a turn for it
    // immediately instead of waiting for the next user turn. Off by default:
    // a wake is a real provider call the user did not ask for. Per-session,
    // resolved at creation; a session never changes its answer mid-life.
    bool background_wake = false;
    // Whether a user turn submitted while the session is PAUSED auto-resumes
    // it. On by default: the message IS the intent to continue. Per-session,
    // resolved at creation like background_wake.
    // (`sessions.pause_auto_resume_on_submit`.)
    bool pause_auto_resume_on_submit = true;
    MemorySettings memory;
    ToolsSettings tools;
    ToolsViewSettings tools_view;
    ProjectContextSettings project_context;
    ProjectLocalSettings project_local;
    ContextContributorSettings context_contributors;
    SessionSettings sessions;
    ServerSettings server;
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
