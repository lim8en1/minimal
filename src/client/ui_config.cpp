#include "minimal/client/ui_config.hpp"

#include <cstdio>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>
#include <system_error>

#include <nlohmann/json.hpp>

#include "minimal/client/config_file_io.hpp"
#include "minimal/common/config_reader.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/client/policy.hpp"

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
    // The UI file degrades instead of refusing: each bad field is warned and
    // keeps its default, through the same strict reader every config uses --
    // so an unknown or misspelled key is named rather than silently ignored.
    // The reader's unknown-key warning already says it is ignored.
    // stderr, like the bootstrap's other diagnostics (this runs before the
    // TUI takes the screen): the TUI's logger is file-only, so a typo in a
    // cosmetic key was ignored with no word but a line in client.log.
    const auto warn = [](const std::string& message) {
        const bool said = message.size() >= 7 && message.compare(message.size() - 7, 7, "ignored") == 0;
        std::fprintf(stderr, "minimal: warning: %s%s\n", message.c_str(), said ? "" : " -- ignored");
    };
    config::Reader reader(parsed, "ui.json", "", warn);
    const auto lenient = [&warn](const std::function<void()>& read) {
        try {
            read();
        } catch (const config::ConfigError& error) {
            warn(error.what());
        }
    };
    // Lenient too: a status_segments that is not an object is warned about
    // and ignored like any bad entry in it, never an escape that ends the
    // TUI (or `minimal status-bar`) over a cosmetic file.
    std::vector<std::pair<std::string, const nlohmann::json*>> segments;
    lenient([&] { segments = reader.Entries("status_segments"); });
    for (const auto& [id, value] : segments) {
        lenient([&] {
            if (value->is_boolean()) {
                if (value->get<bool>()) {
                    reader.Fail("status_segments." + id,
                                "true means nothing here: a segment shows where its plugin "
                                "places it; false hides it, {\"side\", \"order\"} moves it");
                }
                config.status_segments[id] = StatusSegmentOverride{.hidden = true};
                return;
            }
            if (!value->is_object()) {
                reader.Fail("status_segments." + id,
                            "must be false or {\"side\": \"left\"|\"right\", \"order\": int}");
            }
            config::Reader entry = reader.Child(*value, "status_segments." + id);
            StatusSegmentOverride override;
            if (const auto side = entry.MaybeEnum("side", {"left", "right"})) {
                override.side = *side == "left" ? plugins::StatusPlacement::Side::kLeft
                                                : plugins::StatusPlacement::Side::kRight;
            }
            if (const auto order = entry.MaybeInt("order", std::numeric_limits<int>::min(),
                                                  std::numeric_limits<int>::max())) {
                override.order = static_cast<int>(*order);
            }
            entry.Finish(config::UnknownKeys::kWarn);
            config.status_segments[id] = override;
        });
    }
    lenient([&] { config.theme = reader.String("theme", config.theme); });
    const auto bounded_int = [&](const char* key, int& out, int floor) {
        lenient([&] {
            out = static_cast<int>(reader.Int(key, out, floor, std::numeric_limits<int>::max()));
        });
    };
    bounded_int("scrollback_window_max_messages", config.scrollback_window_max_messages, 1);
    // Floored (kMinToolOutputBufferBytes): below the head+marker+tail split's
    // fixed overhead, head_bytes/tail_budget underflowed to a huge size_t and
    // threw std::out_of_range out of the gRPC reader thread on the first delta
    // past the cap.
    bounded_int("tool_output_buffer_bytes", config.tool_output_buffer_bytes,
                kMinToolOutputBufferBytes);
    bounded_int("panel_height", config.panel_height, 3);

    reader.Finish(config::UnknownKeys::kWarn);
    return config;
}

namespace {

// The ONE managed write's shared core: loads ui.json (if present and
// parseable), hands the parsed document to `mutate`, and writes it back
// atomically. Every key the mutator does not touch survives verbatim -- the
// file is the user's, and rewriting it from a default struct would silently
// drop status_segments and the bounded-int knobs.
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

}  // namespace

void SaveThemePreference(const std::filesystem::path& config_dir, const std::string& name) {
    UpdateUiJson(config_dir, "theme preference", [&](nlohmann::json& doc) {
        doc["theme"] = name;
    });
}

}  // namespace minimal::client
