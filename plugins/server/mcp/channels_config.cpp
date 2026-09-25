#include "minimal/mcp/channels_config.hpp"

#include <stdexcept>
#include <unordered_set>

#include "minimal/config/json_config_loader.hpp"
#include "minimal/config/parse_util.hpp"

namespace minimal::config {

namespace {

// No index parameter: the caller catches and labels every failure with the
// channel's key, so this throws bare reasons and never has to know where it
// sits in the object.
ChannelEntry ParseEntry(const std::string& name, const nlohmann::json& entry) {
    if (!entry.is_object()) throw std::runtime_error("must be an object");

    ChannelEntry result;
    result.name = name;
    // The key IS the name: a channel is addressed by its selector half, and a
    // "name" field inside the object would be a second opinion. One rule, one
    // source.
    if (!IsValidSignalName(name)) {
        throw std::runtime_error(
            "channel name '" + name +
            "' must match [a-z0-9._-] (it is the first half of a session's 'channel/signal' "
            "selector)");
    }
    if (name == kReservedInternalChannelName) {
        throw std::runtime_error(
            "channel name '" + name +
            "' is reserved for internal cross-session signals; it is not addressable by config");
    }

    // contains + type checks, not entry.at(): .at() throws a raw
    // json.exception.out_of_range which the blanket catch below would hand to
    // the user verbatim, while every neighbouring field has a hand-written
    // message. Same shape as the mcps.json loader.
    if (entry.contains("mode")) {
        if (!entry.at("mode").is_string()) {
            throw std::runtime_error("mode must be the string \"restricted\" or \"extended\"");
        }
        const std::string mode = entry.at("mode").get<std::string>();
        if (mode == "restricted") {
            result.extended = false;
        } else if (mode == "extended") {
            result.extended = true;
        } else {
            throw std::runtime_error("mode must be \"restricted\" or \"extended\", got '" + mode +
                                     "'");
        }
    }

    if (result.extended) {
        // Extended means "every signal the source emits passes": a signals
        // list there would advertise a filter that does not exist. Refused,
        // not ignored -- a user who wrote one believes a gate is standing.
        if (entry.contains("signals")) {
            throw std::runtime_error(
                "an extended channel must not declare signals (extended means every signal "
                "passes; declare signals only on a restricted channel)");
        }
    } else {
        if (!entry.contains("signals") || !entry.at("signals").is_array() ||
            entry.at("signals").empty()) {
            throw std::runtime_error(
                "a restricted channel must declare a non-empty signals array (or switch to "
                "\"mode\": \"extended\")");
        }
        for (const auto& signal : entry.at("signals")) {
            if (!signal.is_string()) {
                throw std::runtime_error("signals must be an array of strings");
            }
            const std::string parsed = signal.get<std::string>();
            if (!IsValidSignalName(parsed)) {
                throw std::runtime_error("signal '" + parsed +
                                         "' must match [a-z0-9._-]");
            }
            result.signals.push_back(std::move(parsed));
        }
    }

    if (!entry.contains("command") || !entry.at("command").is_string() ||
        entry.at("command").get<std::string>().empty()) {
        throw std::runtime_error("a channel needs a non-empty command (v1 is stdio-only)");
    }
    result.command = entry.at("command").get<std::string>();
    if (entry.contains("args")) {
        // is_array() first: nlohmann's range-for over a non-array value still
        // "works" (a scalar iterates once over itself; an object iterates
        // its VALUES in key-sorted order), so without this check a typo'd
        // object or string silently became a one-element or reordered args
        // list instead of the intended rejection.
        if (!entry.at("args").is_array()) {
            throw std::runtime_error("args must be an array of strings");
        }
        for (const auto& arg : entry.at("args")) {
            if (!arg.is_string()) throw std::runtime_error("args must be an array of strings");
            result.args.push_back(arg.get<std::string>());
        }
    }
    if (entry.contains("env")) result.env = ParseStringMap(entry.at("env"), "env");

    result.enabled = entry.value("enabled", result.enabled);
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
        throw std::runtime_error("channels.json: expected a top-level object keyed by channel name");
    }

    ChannelsConfig config;
    for (const auto& [name, value] : doc.items()) {
        try {
            // JSON objects cannot carry duplicate keys in practice (nlohmann
            // keeps the last), so no duplicate-name check is needed here --
            // unlike the mcps.json array, the container IS the name map.
            config.entries.push_back(ParseEntry(name, value));
        } catch (const std::exception& error) {
            config.errors.push_back("channel '" + name + "': " + error.what());
        }
    }
    return config;
}

ChannelsConfig LoadChannelsConfig(const std::filesystem::path& path) {
    // Absence is the normal "no channels" state, not an error.
    return LoadJsonConfig<ChannelsConfig>(path, "channels", AbsencePolicy::kDefaultOnMissing,
                                          EmptyFilePolicy::kTreatAsMalformed, ParseChannelsConfig);
}

}  // namespace minimal::config
