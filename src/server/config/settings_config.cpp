#include "minimal/config/settings_config.hpp"

#include <cstdint>
#include <limits>

#include "minimal/common/config_reader.hpp"
#include "minimal/common/json_config_loader.hpp"

namespace minimal::config {

namespace {

constexpr std::int64_t kIntMax = std::numeric_limits<int>::max();
constexpr std::int64_t kInt64Max = std::numeric_limits<std::int64_t>::max();

CompactionTriggerSettings ParseCompactionTrigger(Reader& memory) {
    CompactionTriggerSettings result;
    auto trigger = memory.MaybeObject("context_compaction_trigger");
    if (!trigger) return result;
    int kinds = 0;
    if (const auto percent = trigger->MaybeInt("percent", 1, 100)) {
        ++kinds;
        result.kind = CompactionTriggerSettings::Kind::PercentOfWindow;
        result.percent = static_cast<int>(*percent);
    }
    if (const auto tokens = trigger->MaybeInt("tokens", 1)) {
        ++kinds;
        result.kind = CompactionTriggerSettings::Kind::FixedTokens;
        result.tokens = *tokens;
    }
    if (const auto buffer = trigger->MaybeInt("buffer_tokens", 1)) {
        ++kinds;
        result.kind = CompactionTriggerSettings::Kind::WindowBuffer;
        result.buffer_tokens = *buffer;
    }
    trigger->Finish();
    if (kinds != 1) {
        memory.Fail("context_compaction_trigger",
                    "must set exactly one of \"percent\", \"tokens\", \"buffer_tokens\"");
    }
    return result;
}

MemorySettings ParseMemory(Reader& root) {
    MemorySettings result;
    Reader memory = root.Object("memory");
    result.auto_compact = memory.Bool("auto_compact", result.auto_compact);
    result.context_compaction_trigger = ParseCompactionTrigger(memory);
    result.compaction_keep_recent_tokens =
        memory.Int("compaction_keep_recent_tokens", result.compaction_keep_recent_tokens, 0);
    result.compaction_replace_percent = static_cast<int>(
        memory.Int("compaction_replace_percent", result.compaction_replace_percent, 1, 100));
    result.scrollback_page_size =
        memory.Int("scrollback_page_size", result.scrollback_page_size, 0, kIntMax);
    result.compaction_strategy =
        memory.String("compaction_strategy", result.compaction_strategy, /*allow_empty=*/false);
    result.estimate_zero_usage = memory.Bool("estimate_zero_usage", result.estimate_zero_usage);
    memory.Finish();
    return result;
}

ToolsSettings ParseTools(Reader& root) {
    ToolsSettings result;
    Reader tools = root.Object("tools");
    // The pool starts one thread per slot, per session: an absurd value is a
    // resource-exhaustion typo, not a tuning choice.
    result.max_parallel_tool_calls = static_cast<int>(
        tools.Int("max_parallel_tool_calls", result.max_parallel_tool_calls, 1, 64));
    result.max_input_bytes = tools.Int("max_input_bytes", result.max_input_bytes, 0);
    result.max_result_bytes = tools.Int("max_result_bytes", result.max_result_bytes, 0);
    result.max_display_bytes = tools.Int("max_display_bytes", result.max_display_bytes, 0);
    tools.Finish();
    return result;
}

ToolsViewSettings ParseToolsView(Reader& root) {
    ToolsViewSettings result;
    Reader view = root.Object("tools_view");
    result.stdout_buffer_lines =
        view.Int("stdout_buffer_lines", result.stdout_buffer_lines, 0, kIntMax);
    view.Finish();
    return result;
}

ProjectLocalSettings ParseProjectLocal(Reader& root) {
    ProjectLocalSettings result;
    Reader section = root.Object("project_local");
    const std::string mode = section.Enum("mode", "trusted", {"trusted", "all", "none"});
    if (mode == "all") {
        result.mode = ProjectLocalMode::kAll;
    } else if (mode == "none") {
        result.mode = ProjectLocalMode::kNone;
    }
    section.Finish();
    return result;
}

ContextContributorSettings ParseContextContributors(Reader& root) {
    ContextContributorSettings result;
    Reader cc = root.Object("context_contributors");
    result.max_bytes_per_section =
        cc.Int("max_bytes_per_section", result.max_bytes_per_section, 0);
    result.disabled = cc.StringList("disabled");
    cc.Finish();
    return result;
}

LoggingSettings ParseLogging(Reader& root) {
    LoggingSettings result;
    Reader logging = root.Object("logging");
    result.level = logging.Enum("level", result.level,
                                {"trace", "debug", "info", "warn", "error", "critical", "off"});
    result.path = logging.String("path", result.path);
    // 0 MB would rotate on every message; 0 files keeps nothing.
    result.max_size_mb = logging.Int("max_size_mb", result.max_size_mb, 1, 1 << 20);
    result.max_files = logging.Int("max_files", result.max_files, 1, 1000);
    logging.Finish();
    return result;
}

// Session lifetime policy. 0 is meaningful ("never") for the two timeout-shaped
// keys. `initial_attach_guard_seconds` is a safety-net window against a
// non-conforming client, not an optional feature, so 0 has no "never" reading:
// it would terminate every session before a client could attach, and
// SessionRegistry's reaper polls at a quarter of it, so 0 would also spin
// that thread at 100% CPU.
SessionSettings ParseSessions(Reader& root) {
    SessionSettings result;
    Reader sessions = root.Object("sessions");
    result.detached_idle_ttl_seconds =
        sessions.Int("detached_idle_ttl_seconds", result.detached_idle_ttl_seconds, 0);
    result.unattended_prompt_timeout_seconds = sessions.Int(
        "unattended_prompt_timeout_seconds", result.unattended_prompt_timeout_seconds, 0);
    result.initial_attach_guard_seconds =
        sessions.Int("initial_attach_guard_seconds", result.initial_attach_guard_seconds, 1);
    sessions.Finish();
    return result;
}

ServerSettings ParseServer(Reader& root) {
    ServerSettings result;
    Reader server = root.Object("server");
    result.idle_shutdown_seconds =
        server.Int("idle_shutdown_seconds", result.idle_shutdown_seconds, 0);
    result.discovery_warmup_timeout_seconds = server.Int(
        "discovery_warmup_timeout_seconds", result.discovery_warmup_timeout_seconds, 0);
    server.Finish();
    return result;
}

}  // namespace

SettingsConfig ParseSettingsConfig(const nlohmann::json& doc) {
    SettingsConfig config;
    Reader root(doc, "settings.json");
    config.default_agent_preset = root.String("default_agent_preset", config.default_agent_preset);
    config.default_model = root.String("default_model", config.default_model);
    config.small_model = root.String("small_model", config.small_model);
    config.default_subagent_model =
        root.String("default_subagent_model", config.default_subagent_model);
    config.max_concurrent_subagents =
        root.Int("max_concurrent_subagents", config.max_concurrent_subagents, 0, kInt64Max);
    config.background_wake = root.Bool("background_wake", config.background_wake);
    config.pause_auto_resume_on_submit =
        root.Bool("pause_auto_resume_on_submit", config.pause_auto_resume_on_submit);
    config.memory = ParseMemory(root);
    config.tools = ParseTools(root);
    config.tools_view = ParseToolsView(root);
    config.project_local = ParseProjectLocal(root);
    config.context_contributors = ParseContextContributors(root);
    config.sessions = ParseSessions(root);
    config.server = ParseServer(root);
    config.session_db_path = root.String("session_db_path", config.session_db_path);
    config.logging = ParseLogging(root);
    root.Finish();
    return config;
}

SettingsConfig LoadSettingsConfig(const std::filesystem::path& path) {
    return LoadJsonConfig<SettingsConfig>(path, "settings.json", AbsencePolicy::kThrow,
                                          EmptyFilePolicy::kTreatAsMalformed, ParseSettingsConfig);
}

}  // namespace minimal::config
