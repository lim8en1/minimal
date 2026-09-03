#include "minimal/config/hooks_config.hpp"

#include <stdexcept>
#include <string>

#include "minimal/config/json_config_loader.hpp"

namespace minimal::config {
namespace {

[[noreturn]] void Reject(const std::string& what) { throw std::runtime_error(what); }

std::pair<std::string, std::string> ParseFilterEntry(const std::string& on,
                                                     nlohmann::json::const_iterator it) {
    if (!it.value().is_string()) {
        Reject("hooks: action on '" + on + "': filter field '" + it.key() +
               "' must be a string value");
    }
    if (it.key().empty()) {
        Reject("hooks: action on '" + on + "': filter field names must be non-empty");
    }
    return {it.key(), it.value().get<std::string>()};
}

}  // namespace

HooksConfig ParseHooksConfig(const nlohmann::json& doc) {
    HooksConfig config;

    if (!doc.is_object()) Reject("hooks.json: expected a JSON object at the top level");

    if (doc.contains("default_timeout_ms")) {
        const auto& value = doc.at("default_timeout_ms");
        if (!value.is_number_integer()) {
            Reject("hooks.json: default_timeout_ms must be an integer");
        }
        config.default_timeout_ms = value.get<int>();
        if (config.default_timeout_ms < 0) {
            // Silently disabling a safety budget is never the intent; 0 is the
            // explicit opt-out and must be spelled as one.
            Reject("hooks.json: default_timeout_ms must be >= 0");
        }
    }

    if (doc.contains("hooks")) {
        const auto& hooks = doc.at("hooks");
        if (!hooks.is_array()) Reject("hooks.json: 'hooks' must be an array");
        for (const auto& entry : hooks) {
            if (!entry.is_object()) Reject("hooks.json: every hook entry must be an object");

            HookActionConfig action;
            if (!entry.contains("on") || !entry.at("on").is_string() ||
                entry.at("on").get<std::string>().empty()) {
                Reject("hooks.json: every hook entry needs a non-empty string 'on'");
            }
            action.on = entry.at("on").get<std::string>();

            if (!entry.contains("command") || !entry.at("command").is_string() ||
                entry.at("command").get<std::string>().empty()) {
                Reject("hooks.json: hook on '" + action.on +
                       "' needs a non-empty string 'command'");
            }
            action.command = entry.at("command").get<std::string>();

            if (entry.contains("timeout_ms")) {
                const auto& timeout = entry.at("timeout_ms");
                if (!timeout.is_number_integer()) {
                    Reject("hooks.json: hook on '" + action.on +
                           "': timeout_ms must be an integer");
                }
                action.timeout_ms = timeout.get<int>();
                if (*action.timeout_ms < 0) {
                    Reject("hooks.json: hook on '" + action.on +
                           "': timeout_ms must be >= 0");
                }
            }

            if (entry.contains("filter")) {
                const auto& filter = entry.at("filter");
                if (!filter.is_object()) {
                    Reject("hooks.json: hook on '" + action.on + "': 'filter' must be an object");
                }
                for (auto it = filter.begin(); it != filter.end(); ++it) {
                    action.filter.push_back(ParseFilterEntry(action.on, it));
                }
            }

            config.hooks.push_back(std::move(action));
        }
    }

    return config;
}

HooksConfig LoadHooksConfig(const std::filesystem::path& path) {
    // No hooks.json, or an empty one, is the normal no-hooks state -- the only
    // one of the six loaders where an empty-but-present file is absence, not
    // malformed content.
    //
    // Malformed JSON now throws a wrapped std::runtime_error ("hooks.json:
    // invalid JSON: ...") like every other loader, instead of letting a raw
    // nlohmann::json::parse_error escape -- a deliberate Phase 3 behavior
    // change (see hooks_config_test.cpp, updated in the same commit).
    return LoadJsonConfig<HooksConfig>(path, "hooks", AbsencePolicy::kDefaultOnMissing,
                                       EmptyFilePolicy::kTreatAsAbsent, ParseHooksConfig);
}

}  // namespace minimal::config
