#include "minimal/config/settings_config.hpp"

#include <fstream>
#include <stdexcept>

namespace minimal::config {

namespace {

std::int64_t RequirePositiveInt(const nlohmann::json& value, const std::string& what) {
    if (!value.is_number_integer()) {
        throw std::runtime_error("settings.json: memory.context_compaction_trigger." + what +
                                 " must be an integer");
    }
    const std::int64_t number = value.get<std::int64_t>();
    if (number <= 0) {
        throw std::runtime_error("settings.json: memory.context_compaction_trigger." + what +
                                 " must be positive");
    }
    return number;
}

CompactionTriggerSettings ParseCompactionTrigger(const nlohmann::json& memory) {
    CompactionTriggerSettings result;
    if (!memory.contains("context_compaction_trigger")) {
        // Legacy flat key, from before the trigger became a strategy choice.
        // Still honoured so an existing settings.json does not silently stop
        // compacting; 0 keeps its old meaning of "never". The new object wins
        // when both are present.
        const std::int64_t legacy = memory.value("context_compaction_trigger_tokens", 0);
        if (legacy > 0) {
            result.kind = CompactionTriggerSettings::Kind::FixedTokens;
            result.tokens = legacy;
        }
        return result;
    }

    const auto& trigger = memory.at("context_compaction_trigger");
    if (!trigger.is_object() || trigger.size() != 1) {
        throw std::runtime_error("settings.json: memory.context_compaction_trigger must be an "
                                 "object with exactly one of 'percent', 'tokens', 'buffer_tokens'");
    }
    const auto [key, value] = *trigger.items().begin();
    if (key == "percent") {
        const std::int64_t percent = RequirePositiveInt(value, "percent");
        if (percent > 100) {
            throw std::runtime_error(
                "settings.json: memory.context_compaction_trigger.percent must be at most 100");
        }
        result.kind = CompactionTriggerSettings::Kind::PercentOfWindow;
        result.percent = static_cast<int>(percent);
    } else if (key == "tokens") {
        result.kind = CompactionTriggerSettings::Kind::FixedTokens;
        result.tokens = RequirePositiveInt(value, "tokens");
    } else if (key == "buffer_tokens") {
        result.kind = CompactionTriggerSettings::Kind::WindowBuffer;
        result.buffer_tokens = RequirePositiveInt(value, "buffer_tokens");
    } else {
        throw std::runtime_error("settings.json: memory.context_compaction_trigger has unknown key '" +
                                 key + "'; expected exactly one of 'percent', 'tokens', 'buffer_tokens'");
    }
    return result;
}

MemorySettings ParseMemory(const nlohmann::json& doc) {
    MemorySettings result;
    if (!doc.contains("memory")) return result;
    const auto& memory = doc.at("memory");
    result.auto_compact = memory.value("auto_compact", result.auto_compact);
    result.context_compaction_trigger = ParseCompactionTrigger(memory);
    result.compaction_keep_recent_tokens =
        memory.value("compaction_keep_recent_tokens", result.compaction_keep_recent_tokens);
    result.scrollback_page_size = memory.value("scrollback_page_size", result.scrollback_page_size);
    result.compaction_strategy = memory.value("compaction_strategy", result.compaction_strategy);
    return result;
}

ToolsSettings ParseTools(const nlohmann::json& doc) {
    ToolsSettings result;
    if (!doc.contains("tools")) return result;
    const auto& tools = doc.at("tools");
    result.read = tools.value("read", result.read);
    result.edit = tools.value("edit", result.edit);
    result.bash = tools.value("bash", result.bash);
    if (tools.contains("mcp") && tools.at("mcp").is_object()) {
        const auto& mcp = tools.at("mcp");
        result.mcp.connect_timeout_ms = mcp.value("connect_timeout_ms", result.mcp.connect_timeout_ms);
        result.mcp.request_timeout_ms = mcp.value("request_timeout_ms", result.mcp.request_timeout_ms);
        result.mcp.max_list_pages = mcp.value("max_list_pages", result.mcp.max_list_pages);
        if (result.mcp.max_list_pages < 1) {
            throw std::runtime_error(
                "settings.json: tools.mcp.max_list_pages must be at least 1");
        }
        result.mcp.restart_cooldown_ms =
            mcp.value("restart_cooldown_ms", result.mcp.restart_cooldown_ms);
        if (result.mcp.restart_cooldown_ms < 0) {
            throw std::runtime_error(
                "settings.json: tools.mcp.restart_cooldown_ms must not be negative");
        }
    }
    result.max_parallel_tool_calls =
        tools.value("max_parallel_tool_calls", result.max_parallel_tool_calls);
    if (result.max_parallel_tool_calls < 1) {
        throw std::runtime_error(
            "settings.json: tools.max_parallel_tool_calls must be at least 1");
    }
    result.max_input_bytes = tools.value("max_input_bytes", result.max_input_bytes);
    if (result.max_input_bytes < 0) {
        throw std::runtime_error(
            "settings.json: tools.max_input_bytes must not be negative");
    }
    result.max_result_bytes = tools.value("max_result_bytes", result.max_result_bytes);
    if (result.max_result_bytes < 0) {
        throw std::runtime_error(
            "settings.json: tools.max_result_bytes must not be negative");
    }
    return result;
}

ToolsViewSettings ParseToolsView(const nlohmann::json& doc) {
    ToolsViewSettings result;
    if (!doc.contains("tools_view")) return result;
    result.stdout_buffer_lines = doc.at("tools_view").value("stdout_buffer_lines", result.stdout_buffer_lines);
    return result;
}

ProjectContextSettings ParseProjectContext(const nlohmann::json& doc) {
    ProjectContextSettings result;
    if (!doc.contains("project_context")) return result;
    const auto& pc = doc.at("project_context");
    if (!pc.is_object()) {
        throw std::runtime_error("settings.json: project_context must be an object");
    }
    result.enabled = pc.value("enabled", result.enabled);
    result.search_ancestors = pc.value("search_ancestors", result.search_ancestors);
    result.max_bytes = pc.value("max_bytes", result.max_bytes);
    if (result.max_bytes < 0) {
        throw std::runtime_error("settings.json: project_context.max_bytes must not be negative");
    }
    if (pc.contains("files")) {
        const auto& files = pc.at("files");
        if (!files.is_array()) {
            throw std::runtime_error(
                "settings.json: project_context.files must be an array of file names");
        }
        result.files.clear();
        for (const auto& entry : files) {
            if (!entry.is_string() || entry.get<std::string>().empty()) {
                throw std::runtime_error(
                    "settings.json: project_context.files must contain non-empty strings");
            }
            // File NAMES, not paths: the loader composes them against
            // project_root (and, with search_ancestors, its ancestors). A
            // separator would silently redefine the search -- "settings in
            // directories I chose" instead of "the convention file" -- and an
            // absolute path would not follow the session at all.
            const std::string name = entry.get<std::string>();
            if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {
                throw std::runtime_error("settings.json: project_context.files entries must be "
                                         "plain file names, not paths ('" +
                                         name + "')");
            }
            result.files.push_back(name);
        }
    }
    return result;
}

LoggingSettings ParseLogging(const nlohmann::json& doc) {
    LoggingSettings result;
    if (!doc.contains("logging")) return result;
    const auto& logging = doc.at("logging");
    result.level = logging.value("level", result.level);
    result.path = logging.value("path", result.path);
    result.max_size_mb = logging.value("max_size_mb", result.max_size_mb);
    result.max_files = logging.value("max_files", result.max_files);
    return result;
}

// Session lifetime policy. Negative values are the one misconfiguration the
// parser refuses: 0 is meaningful ("never") everywhere, so a negative is a
// typo nobody meant, and silently clamping it to 0 would invert the policy.
SessionSettings ParseSessions(const nlohmann::json& doc) {
    SessionSettings result;
    if (!doc.contains("sessions")) return result;
    const auto& sessions = doc.at("sessions");
    result.detached_idle_ttl_seconds =
        sessions.value("detached_idle_ttl_seconds", result.detached_idle_ttl_seconds);
    result.unattended_prompt_timeout_seconds = sessions.value(
        "unattended_prompt_timeout_seconds", result.unattended_prompt_timeout_seconds);
    result.initial_attach_guard_seconds =
        sessions.value("initial_attach_guard_seconds", result.initial_attach_guard_seconds);
    for (const auto* key :
         {"detached_idle_ttl_seconds", "unattended_prompt_timeout_seconds",
          "initial_attach_guard_seconds"}) {
        if (sessions.value(key, std::int64_t{0}) < 0) {
            throw std::runtime_error(
                std::string("settings.json: sessions.") + key + " must not be negative (0 = never)");
        }
    }
    return result;
}

ServerSettings ParseServer(const nlohmann::json& doc) {
    ServerSettings result;
    if (!doc.contains("server")) return result;
    const auto& server = doc.at("server");
    result.idle_shutdown_seconds =
        server.value("idle_shutdown_seconds", result.idle_shutdown_seconds);
    if (result.idle_shutdown_seconds < 0) {
        throw std::runtime_error(
            "settings.json: server.idle_shutdown_seconds must not be negative (0 = never)");
    }
    return result;
}

}  // namespace

SettingsConfig ParseSettingsConfig(const nlohmann::json& doc) {
    SettingsConfig config;
    try {
        config.default_agent_preset = doc.value("default_agent_preset", config.default_agent_preset);
        config.default_model = doc.value("default_model", config.default_model);
        config.default_theme = doc.value("default_theme", config.default_theme);
        config.max_concurrent_subagents = doc.value("max_concurrent_subagents", config.max_concurrent_subagents);
        config.background_wake = doc.value("background_wake", config.background_wake);
        config.memory = ParseMemory(doc);
        config.tools = ParseTools(doc);
        config.tools_view = ParseToolsView(doc);
        config.project_context = ParseProjectContext(doc);
        config.sessions = ParseSessions(doc);
        config.server = ParseServer(doc);
        config.session_db_path = doc.value("session_db_path", config.session_db_path);
        config.logging = ParseLogging(doc);
    } catch (const nlohmann::json::exception& error) {
        throw std::runtime_error(std::string("settings.json: ") + error.what());
    }
    return config;
}

SettingsConfig LoadSettingsConfig(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("settings.json not found: " + path.string());
    nlohmann::json doc;
    try {
        in >> doc;
    } catch (const nlohmann::json::exception& error) {
        throw std::runtime_error("settings.json: invalid JSON: " + std::string(error.what()));
    }
    return ParseSettingsConfig(doc);
}

}  // namespace minimal::config
