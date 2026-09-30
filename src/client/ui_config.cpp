#include "minimal/client/ui_config.hpp"

#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <system_error>

#include <nlohmann/json.hpp>

#include "minimal/client/config_file_io.hpp"
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
    bounded_int("panel_height", config.panel_height, 3);

    // The fold defaults: an enum-as-string ("auto"/"expanded"/"collapsed"),
    // not a bare int -- users hand-edit this file, and "expanded" is
    // self-describing where 1 is not. Unparsable values are warned and left
    // at auto, the same degrade rule as every other bad value here.
    const auto fold_value = [&parsed](const char* key, int& out) {
        if (auto it = parsed.find(key); it != parsed.end()) {
            if (it->is_string()) {
                const auto name = it->get<std::string>();
                if (name == "auto") out = kFoldDefaultAuto;
                else if (name == "expanded") out = kFoldExpanded;
                else if (name == "collapsed") out = kFoldCollapsed;
                else {
                    common::DefaultLogger().warn(
                        "ui.json: \"{}\" must be \"auto\", \"expanded\" or "
                        "\"collapsed\" -- ignored", key);
                }
            } else {
                common::DefaultLogger().warn(
                    "ui.json: \"{}\" must be a string (\"auto\", \"expanded\" or "
                    "\"collapsed\") -- ignored", key);
            }
        }
    };
    fold_value("fold_thinking", config.fold_thinking);
    fold_value("fold_tools", config.fold_tools);
    return config;
}

namespace {

// The ONE managed write's shared core: loads ui.json (if present and
// parseable), hands the parsed document to `mutate`, and writes it back
// atomically. Every key the mutator does not touch survives verbatim -- the
// file is the user's, and rewriting it from a default struct would silently
// drop status_format and the bounded-int knobs.
//
// Refuses to run on a file we cannot READ (permission, transient I/O) or
// cannot PARSE: bytes we cannot understand are the user's, and silently
// replacing them would destroy their config. Returns false (warned) instead
// -- the caller's in-session change has already applied; only the restart
// persistence is skipped.
bool UpdateUiJson(const std::filesystem::path& config_dir, const char* what,
                  const std::function<void(nlohmann::json&)>& mutate) {
    const auto path = config_dir / "ui.json";
    nlohmann::json doc = nlohmann::json::object();
    std::string existing;
    {
        std::error_code ec;
        if (std::filesystem::exists(path, ec) && !TryReadConfigFile(path, &existing)) {
            // Exists but could not be read: never treat that as "empty" --
            // the write below would otherwise replace the user's real
            // (merely unreadable) file with a fresh, near-empty document.
            common::DefaultLogger().warn(
                "ui.json exists but could not be read -- {} NOT persisted; "
                "fix its permissions to enable persistence", what);
            return false;
        }
    }
    if (!existing.empty()) {
        auto parsed = nlohmann::json::parse(existing, nullptr, /*allow_exceptions=*/false);
        if (parsed.is_discarded() || !parsed.is_object()) {
            // Same discipline as client-plugins.json's malformed handling.
            common::DefaultLogger().warn(
                "ui.json is malformed -- {} NOT persisted; fix or remove "
                "the file to enable persistence", what);
            return false;
        }
        doc = std::move(parsed);
    }
    mutate(doc);

    // Atomic write: serialize to a sibling tmp file, then rename over the
    // original. A crash mid-write leaves ui.json.tmp behind (harmless) rather
    // than a truncated ui.json (which LoadUiConfig would degrade to defaults,
    // dropping the user's other keys).
    std::filesystem::create_directories(config_dir);
    if (!WriteConfigFileAtomic(path, doc.dump(2) + "\n")) {
        common::DefaultLogger().warn("could not write {} -- {} not persisted",
                                     path.string(), what);
        return false;
    }
    return true;
}

// "expanded"/"collapsed"/"auto" for kFoldExpanded/kFoldCollapsed/kFoldDefaultAuto.
std::string FoldName(int value) {
    switch (value) {
        case kFoldExpanded: return "expanded";
        case kFoldCollapsed: return "collapsed";
        default: return "auto";
    }
}

}  // namespace

void SaveThemePreference(const std::filesystem::path& config_dir, const std::string& name) {
    UpdateUiJson(config_dir, "theme preference", [&](nlohmann::json& doc) {
        doc["theme"] = name;
    });
}

bool SaveFoldDefaults(const std::filesystem::path& config_dir,
                      std::optional<int> thinking, std::optional<int> tools) {
    return UpdateUiJson(config_dir, "fold defaults", [&](nlohmann::json& doc) {
        if (thinking.has_value()) doc["fold_thinking"] = FoldName(*thinking);
        if (tools.has_value()) doc["fold_tools"] = FoldName(*tools);
    });
}

}  // namespace minimal::client
