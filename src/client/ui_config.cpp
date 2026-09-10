#include "minimal/client/ui_config.hpp"

#include <fstream>
#include <iterator>
#include <system_error>

#include <nlohmann/json.hpp>

#include "minimal/common/logger.hpp"

namespace minimal::client {

UiConfig LoadUiConfig(const std::filesystem::path& config_dir) {
    UiConfig config;
    const auto path = config_dir / "ui.json";
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return config;  // fresh config dir: defaults

    std::ifstream in(path);
    if (!in) {
        common::DefaultLogger().warn("ui.json exists but cannot be read: {} -- using defaults",
                                     path.string());
        return config;
    }
    const std::string contents((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());

    const auto parsed = nlohmann::json::parse(contents, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        common::DefaultLogger().warn(
            "ui.json is malformed (not a JSON object) -- using default UI settings");
        return config;
    }
    if (auto it = parsed.find("status_format"); it != parsed.end() && it->is_string()) {
        config.status_format = it->get<std::string>();
    } else if (it != parsed.end()) {
        common::DefaultLogger().warn("ui.json: \"status_format\" must be a string -- ignored");
    }
    if (auto it = parsed.find("theme"); it != parsed.end() && it->is_string()) {
        config.theme = it->get<std::string>();
    } else if (it != parsed.end()) {
        common::DefaultLogger().warn("ui.json: \"theme\" must be a string -- ignored");
    }
    const auto bounded_int = [&parsed](const char* key, int& out, int floor) {
        if (auto it = parsed.find(key); it != parsed.end()) {
            if (it->is_number_integer() && it->get<int>() >= floor) {
                out = it->get<int>();
            } else {
                common::DefaultLogger().warn(
                    "ui.json: \"{}\" must be an integer >= {} -- ignored", key, floor);
            }
        }
    };
    bounded_int("scrollback_window_max_messages", config.scrollback_window_max_messages, 1);
    // A floor well above the head+marker+tail split's fixed overhead
    // (SessionState::OnToolOutputDelta's truncation marker alone is ~70
    // bytes): a value below it made head_bytes/tail_budget underflow to a
    // huge size_t and threw std::out_of_range out of the gRPC reader thread
    // on the first delta past the cap.
    bounded_int("tool_output_buffer_bytes", config.tool_output_buffer_bytes, 4096);
    bounded_int("tools_panel_stdout_buffer_lines", config.tools_panel_stdout_buffer_lines, 1);
    return config;
}

void SaveThemePreference(const std::filesystem::path& config_dir, const std::string& name) {
    const auto path = config_dir / "ui.json";
    // Round-trip the WHOLE document, not a synthetic {theme: ...}: every other
    // key in the file is the user's, and rewriting ui.json from a default
    // struct would silently drop status_format and the bounded-int knobs.
    nlohmann::json doc = nlohmann::json::object();
    std::string existing;
    {
        std::error_code ec;
        if (std::filesystem::exists(path, ec)) {
            std::ifstream in(path);
            if (in) {
                existing = std::string((std::istreambuf_iterator<char>(in)),
                                       std::istreambuf_iterator<char>());
            }
        }
    }
    if (!existing.empty()) {
        auto parsed = nlohmann::json::parse(existing, nullptr, /*allow_exceptions=*/false);
        if (parsed.is_discarded() || !parsed.is_object()) {
            // Same discipline as client-plugins.json's malformed handling: the
            // file exists but we cannot understand it, so we must not replace
            // the user's bytes with ours. The in-session theme still applied;
            // only the restart persistence is skipped.
            common::DefaultLogger().warn(
                "ui.json is malformed -- theme preference NOT persisted; fix or remove "
                "the file to enable persistence");
            return;
        }
        doc = std::move(parsed);
    }
    doc["theme"] = name;

    // Atomic-ish write: serialize to a sibling tmp file, then rename over the
    // original. A crash mid-write leaves ui.json.tmp behind (harmless) rather
    // than a truncated ui.json (which LoadUiConfig would degrade to defaults,
    // dropping the user's other keys).
    std::filesystem::create_directories(config_dir);
    const auto tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp);
        if (!out) {
            common::DefaultLogger().warn("could not write {} -- theme preference not "
                                         "persisted",
                                         tmp);
            return;
        }
        out << doc.dump(2) << "\n";
        if (!out) {
            common::DefaultLogger().warn("could not write {} -- theme preference not "
                                         "persisted",
                                         tmp);
            return;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        common::DefaultLogger().warn("could not replace {} with {} -- theme preference not "
                                     "persisted ({})",
                                     path.string(), tmp, ec.message());
    }
}

}  // namespace minimal::client
