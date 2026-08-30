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
    return result;
}

ToolsSettings ParseTools(const nlohmann::json& doc) {
    ToolsSettings result;
    if (!doc.contains("tools")) return result;
    const auto& tools = doc.at("tools");
    result.read = tools.value("read", result.read);
    result.edit = tools.value("edit", result.edit);
    result.bash = tools.value("bash", result.bash);
    return result;
}

ToolsViewSettings ParseToolsView(const nlohmann::json& doc) {
    ToolsViewSettings result;
    if (!doc.contains("tools_view")) return result;
    result.stdout_buffer_lines = doc.at("tools_view").value("stdout_buffer_lines", result.stdout_buffer_lines);
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

}  // namespace

SettingsConfig ParseSettingsConfig(const nlohmann::json& doc) {
    SettingsConfig config;
    try {
        config.default_agent_preset = doc.value("default_agent_preset", config.default_agent_preset);
        config.default_model = doc.value("default_model", config.default_model);
        config.default_theme = doc.value("default_theme", config.default_theme);
        config.max_concurrent_subagents = doc.value("max_concurrent_subagents", config.max_concurrent_subagents);
        config.memory = ParseMemory(doc);
        config.tools = ParseTools(doc);
        config.tools_view = ParseToolsView(doc);
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
