#include "minimal/config/settings_config.hpp"

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <type_traits>

#include "minimal/config/json_config_loader.hpp"

namespace minimal::config {

namespace {

// A settings value with a type check that NAMES the key. nlohmann's value()
// throws "[json.exception.type_error.302] type must be string, but is number"
// -- no key, no file section -- and silently wraps an out-of-range integer into
// a narrower type. An absent key is the fallback; a present key of the wrong
// type, or an integer that does not fit, is a named error.
template <typename T>
T GetField(const nlohmann::json& object, std::string_view section, std::string_view key,
           const T& fallback) {
    const auto it = object.find(std::string(key));
    if (it == object.end()) return fallback;
    const std::string where =
        "settings.json: " + (section.empty() ? std::string() : std::string(section) + ".") +
        std::string(key);
    if constexpr (std::is_same_v<T, bool>) {
        if (!it->is_boolean()) {
            throw std::runtime_error(where + " must be true or false, got " + it->dump());
        }
        return it->get<bool>();
    } else if constexpr (std::is_integral_v<T>) {
        if (!it->is_number_integer()) {
            throw std::runtime_error(where + " must be an integer, got " + it->dump());
        }
        const auto wide = it->get<std::int64_t>();
        if (wide < static_cast<std::int64_t>(std::numeric_limits<T>::min()) ||
            (wide > 0 && static_cast<std::uint64_t>(wide) >
                             static_cast<std::uint64_t>(std::numeric_limits<T>::max()))) {
            throw std::runtime_error(where + " is out of range: " + it->dump());
        }
        return static_cast<T>(wide);
    } else if constexpr (std::is_same_v<T, std::string>) {
        if (!it->is_string()) {
            throw std::runtime_error(where + " must be a string, got " + it->dump());
        }
        return it->get<std::string>();
    } else {
        return it->get<T>();
    }
}


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
        if (memory.contains("context_compaction_trigger_tokens")) {
            // Explicit reads, not value()-with-default: a wrong-typed or
            // negative legacy value must fail loudly like the new-object path
            // does, not silently fall out as Kind::Never ("never compact")
            // -- the one misconfiguration whose symptom is a context that
            // quietly grows until the backend starts rejecting requests.
            const auto& legacy_value = memory.at("context_compaction_trigger_tokens");
            if (!legacy_value.is_number_integer()) {
                throw std::runtime_error(
                    "settings.json: memory.context_compaction_trigger_tokens must be an integer");
            }
            const std::int64_t legacy = legacy_value.get<std::int64_t>();
            if (legacy < 0) {
                throw std::runtime_error(
                    "settings.json: memory.context_compaction_trigger_tokens must not be "
                    "negative");
            }
            if (legacy > 0) {
                result.kind = CompactionTriggerSettings::Kind::FixedTokens;
                result.tokens = legacy;
            }
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
    result.auto_compact = GetField(memory, "memory", "auto_compact", result.auto_compact);
    result.context_compaction_trigger = ParseCompactionTrigger(memory);
    result.compaction_keep_recent_tokens =
        GetField(memory, "memory", "compaction_keep_recent_tokens", result.compaction_keep_recent_tokens);
    if (memory.contains("compaction_replace_percent")) {
        const auto& value = memory.at("compaction_replace_percent");
        const std::int64_t percent = value.is_number_integer() ? value.get<std::int64_t>() : 0;
        if (percent < 1 || percent > 100) {
            throw std::runtime_error(
                "settings.json: memory.compaction_replace_percent must be between 1 and 100");
        }
        result.compaction_replace_percent = static_cast<int>(percent);
    }
    result.scrollback_page_size = GetField(memory, "memory", "scrollback_page_size", result.scrollback_page_size);
    result.compaction_strategy = GetField(memory, "memory", "compaction_strategy", result.compaction_strategy);
    result.estimate_zero_usage = GetField(memory, "memory", "estimate_zero_usage", result.estimate_zero_usage);
    return result;
}

ToolsSettings ParseTools(const nlohmann::json& doc) {
    ToolsSettings result;
    if (!doc.contains("tools")) return result;
    const auto& tools = doc.at("tools");
    result.raw = tools;
    result.max_parallel_tool_calls =
        GetField(tools, "tools", "max_parallel_tool_calls", result.max_parallel_tool_calls);
    // The pool starts one thread per slot, per session: an absurd value is a
    // resource-exhaustion typo, not a tuning choice.
    if (result.max_parallel_tool_calls < 1 || result.max_parallel_tool_calls > 64) {
        throw std::runtime_error(
            "settings.json: tools.max_parallel_tool_calls must be between 1 and 64");
    }
    result.max_input_bytes = GetField(tools, "tools", "max_input_bytes", result.max_input_bytes);
    if (result.max_input_bytes < 0) {
        throw std::runtime_error(
            "settings.json: tools.max_input_bytes must not be negative");
    }
    result.max_result_bytes = GetField(tools, "tools", "max_result_bytes", result.max_result_bytes);
    if (result.max_result_bytes < 0) {
        throw std::runtime_error(
            "settings.json: tools.max_result_bytes must not be negative");
    }
    result.max_display_bytes = GetField(tools, "tools", "max_display_bytes", result.max_display_bytes);
    if (result.max_display_bytes < 0) {
        throw std::runtime_error(
            "settings.json: tools.max_display_bytes must not be negative");
    }
    return result;
}

ToolsViewSettings ParseToolsView(const nlohmann::json& doc) {
    ToolsViewSettings result;
    if (!doc.contains("tools_view")) return result;
    result.stdout_buffer_lines = GetField(doc.at("tools_view"), "tools_view", "stdout_buffer_lines", result.stdout_buffer_lines);
    return result;
}

ProjectContextSettings ParseProjectContext(const nlohmann::json& doc) {
    ProjectContextSettings result;
    if (!doc.contains("project_context")) return result;
    const auto& pc = doc.at("project_context");
    if (!pc.is_object()) {
        throw std::runtime_error("settings.json: project_context must be an object");
    }
    result.enabled = GetField(pc, "project_context", "enabled", result.enabled);
    result.search_ancestors = GetField(pc, "project_context", "search_ancestors", result.search_ancestors);
    result.max_bytes = GetField(pc, "project_context", "max_bytes", result.max_bytes);
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

ContextContributorSettings ParseContextContributors(const nlohmann::json& doc) {
    ContextContributorSettings result;
    if (!doc.contains("context_contributors")) return result;
    const auto& cc = doc.at("context_contributors");
    if (!cc.is_object()) {
        throw std::runtime_error("settings.json: context_contributors must be an object");
    }
    result.max_bytes_per_section = GetField(cc, "context_contributors", "max_bytes_per_section", result.max_bytes_per_section);
    if (result.max_bytes_per_section < 0) {
        throw std::runtime_error(
            "settings.json: context_contributors.max_bytes_per_section must not be negative");
    }
    if (cc.contains("disabled")) {
        const auto& disabled = cc.at("disabled");
        if (!disabled.is_array()) {
            throw std::runtime_error(
                "settings.json: context_contributors.disabled must be an array of plugin names");
        }
        for (const auto& entry : disabled) {
            if (!entry.is_string() || entry.get<std::string>().empty()) {
                throw std::runtime_error(
                    "settings.json: context_contributors.disabled must contain non-empty strings");
            }
            result.disabled.push_back(entry.get<std::string>());
        }
    }
    return result;
}

LoggingSettings ParseLogging(const nlohmann::json& doc) {
    LoggingSettings result;
    if (!doc.contains("logging")) return result;
    const auto& logging = doc.at("logging");
    result.level = GetField(logging, "logging", "level", result.level);
    result.path = GetField(logging, "logging", "path", result.path);
    result.max_size_mb = GetField(logging, "logging", "max_size_mb", result.max_size_mb);
    result.max_files = GetField(logging, "logging", "max_files", result.max_files);
    return result;
}

// Session lifetime policy. Negative values are the one misconfiguration the
// parser refuses: 0 is meaningful ("never") for the two timeout-shaped keys,
// so a negative is a typo nobody meant, and silently clamping it to 0 would
// invert the policy. `initial_attach_guard_seconds` is NOT in that family --
// it is a safety-net window against a non-conforming client, not an optional
// feature, so 0 has no "never" reading: it would terminate every session
// before a client could possibly attach, and SessionRegistry's reaper polls
// at a quarter of it, so 0 also spins that thread at 100% CPU. Rejected
// alongside negative rather than silently reinterpreted.
SessionSettings ParseSessions(const nlohmann::json& doc) {
    SessionSettings result;
    if (!doc.contains("sessions")) return result;
    const auto& sessions = doc.at("sessions");
    result.detached_idle_ttl_seconds =
        GetField(sessions, "sessions", "detached_idle_ttl_seconds", result.detached_idle_ttl_seconds);
    result.unattended_prompt_timeout_seconds = GetField(sessions, "sessions", 
        "unattended_prompt_timeout_seconds", result.unattended_prompt_timeout_seconds);
    result.initial_attach_guard_seconds =
        GetField(sessions, "sessions", "initial_attach_guard_seconds", result.initial_attach_guard_seconds);
    for (const auto* key : {"detached_idle_ttl_seconds", "unattended_prompt_timeout_seconds"}) {
        if (GetField(sessions, "sessions", key, std::int64_t{0}) < 0) {
            throw std::runtime_error(
                std::string("settings.json: sessions.") + key + " must not be negative (0 = never)");
        }
    }
    if (result.initial_attach_guard_seconds <= 0) {
        throw std::runtime_error(
            "settings.json: sessions.initial_attach_guard_seconds must be positive (it has no "
            "\"never\" reading -- see the loader comment)");
    }
    return result;
}

ServerSettings ParseServer(const nlohmann::json& doc) {
    ServerSettings result;
    if (!doc.contains("server")) return result;
    const auto& server = doc.at("server");
    result.idle_shutdown_seconds =
        GetField(server, "server", "idle_shutdown_seconds", result.idle_shutdown_seconds);
    if (result.idle_shutdown_seconds < 0) {
        throw std::runtime_error(
            "settings.json: server.idle_shutdown_seconds must not be negative (0 = never)");
    }
    result.discovery_warmup_timeout_seconds = GetField(server, "server", 
        "discovery_warmup_timeout_seconds", result.discovery_warmup_timeout_seconds);
    if (result.discovery_warmup_timeout_seconds < 0) {
        throw std::runtime_error("settings.json: server.discovery_warmup_timeout_seconds must not "
                                 "be negative (0 = disables the warm-up)");
    }
    return result;
}

}  // namespace

SettingsConfig ParseSettingsConfig(const nlohmann::json& doc) {
    SettingsConfig config;
    try {
        config.default_agent_preset = GetField(doc, "", "default_agent_preset", config.default_agent_preset);
        config.default_model = GetField(doc, "", "default_model", config.default_model);
        config.small_model = GetField(doc, "", "small_model", config.small_model);
        config.default_subagent_model =
            GetField(doc, "", "default_subagent_model", config.default_subagent_model);
        config.default_theme = GetField(doc, "", "default_theme", config.default_theme);
        config.max_concurrent_subagents = GetField(doc, "", "max_concurrent_subagents", config.max_concurrent_subagents);
        config.background_wake = GetField(doc, "", "background_wake", config.background_wake);
        config.pause_auto_resume_on_submit =
            GetField(doc, "", "pause_auto_resume_on_submit", config.pause_auto_resume_on_submit);
        config.memory = ParseMemory(doc);
        config.tools = ParseTools(doc);
        config.tools_view = ParseToolsView(doc);
        config.project_context = ParseProjectContext(doc);
        config.context_contributors = ParseContextContributors(doc);
        config.sessions = ParseSessions(doc);
        config.server = ParseServer(doc);
        config.session_db_path = GetField(doc, "", "session_db_path", config.session_db_path);
        config.logging = ParseLogging(doc);
    } catch (const nlohmann::json::exception& error) {
        throw std::runtime_error(std::string("settings.json: ") + error.what());
    }
    return config;
}

SettingsConfig LoadSettingsConfig(const std::filesystem::path& path) {
    return LoadJsonConfig<SettingsConfig>(path, "settings", AbsencePolicy::kThrow,
                                          EmptyFilePolicy::kTreatAsMalformed, ParseSettingsConfig);
}

}  // namespace minimal::config
