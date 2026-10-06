#include "minimal/mcp/channels_config.hpp"

#include <unordered_set>

#include "minimal/common/json_config_loader.hpp"
#include "minimal/common/config_reader.hpp"

namespace minimal::config {

namespace {

ChannelEntry ParseEntry(const std::string& name, Reader& entry) {
    ChannelEntry result;
    result.name = name;
    // The key IS the name: a channel is addressed by its selector half, and a
    // "name" field inside the object would be a second opinion.
    if (!IsValidSignalName(name)) {
        entry.Fail("", "channel name must match [a-z0-9._-] (it is the first half of a "
                       "session's 'channel/signal' selector)");
    }
    if (name == kReservedInternalChannelName) {
        entry.Fail("", "channel name is reserved for internal cross-session signals");
    }
    result.extended = entry.Enum("mode", "restricted", {"restricted", "extended"}) == "extended";
    if (result.extended) {
        // Extended means "every signal the source emits passes": a signals
        // list there would advertise a filter that does not exist.
        if (entry.Has("signals")) {
            entry.Fail("signals", "an extended channel must not declare signals (extended means "
                                  "every signal passes)");
        }
    } else {
        result.signals = entry.StringList("signals");
        if (result.signals.empty()) {
            entry.Fail("signals", "a restricted channel must declare a non-empty signals list "
                                  "(or switch to \"mode\": \"extended\")");
        }
        for (const auto& signal : result.signals) {
            if (!IsValidSignalName(signal)) {
                entry.Fail("signals", "signal '" + signal + "' must match [a-z0-9._-]");
            }
        }
    }
    result.command = entry.RequireString("command");
    result.args = entry.StringList("args", /*allow_empty_entries=*/true);
    if (auto env = entry.MaybeStringMap("env")) result.env = {env->begin(), env->end()};
    result.enabled = entry.Bool("enabled", result.enabled);
    entry.Finish();
    return result;
}

}  // namespace

const ChannelEntry* ChannelsConfig::Find(const std::string& name) const {
    for (const auto& entry : entries) {
        if (entry.name == name) return &entry;
    }
    return nullptr;
}

ChannelsConfig ParseChannelsConfig(const nlohmann::json& doc) {
    if (!doc.is_object()) {
        throw ConfigError("channels.json: expected a top-level object keyed by channel name");
    }
    ChannelsConfig config;
    for (const auto& [name, value] : doc.items()) {
        if (IsCommentKey(name)) continue;
        // A bad entry degrades to a recorded reason; the good ones survive.
        try {
            if (!value.is_object()) throw ConfigError("channels.json: " + name + ": must be an object");
            Reader reader(value, "channels.json", name);
            config.entries.push_back(ParseEntry(name, reader));
        } catch (const ConfigError& error) {
            config.errors.push_back(error.what());
        }
    }
    return config;
}

ChannelsConfig LoadChannelsConfig(const std::filesystem::path& path) {
    // Absence is the normal "no channels" state, not an error.
    return LoadJsonConfig<ChannelsConfig>(path, "channels.json", AbsencePolicy::kDefaultOnMissing,
                                          EmptyFilePolicy::kTreatAsMalformed, ParseChannelsConfig);
}

}  // namespace minimal::config
