#include "minimal/client/ui_config.hpp"

#include <fstream>
#include <iterator>

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
    const auto positive_int = [&parsed](const char* key, int& out) {
        if (auto it = parsed.find(key); it != parsed.end()) {
            if (it->is_number_integer() && it->get<int>() > 0) {
                out = it->get<int>();
            } else {
                common::DefaultLogger().warn(
                    "ui.json: \"{}\" must be a positive integer -- ignored", key);
            }
        }
    };
    positive_int("scrollback_window_max_messages", config.scrollback_window_max_messages);
    positive_int("tool_output_buffer_bytes", config.tool_output_buffer_bytes);
    return config;
}

}  // namespace minimal::client
