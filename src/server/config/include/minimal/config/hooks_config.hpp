#pragma once

// hooks.json -- the user-configurable half of the hook system (track O).
//
// A hook ACTION maps a shell command onto a HOOK POINT: `on` names the point,
// `filter` narrows which emissions it answers, `command` is what runs. The
// point itself is code -- core registers the conversation-path points, plugins
// register their own -- and this file is pure data against those names, the
// same relationship keybindings.json has to client views.
//
// Sibling of permissions.json deliberately: a hook action is shell code the
// USER opted into, and a reviewer should find it next to the other
// behavior-governing file, not buried in settings. Parsed strictly (like
// permissions.json, unlike plugins.json): a typo silently disabling a guard
// hook is worse than a refused start, and the sanity check is what reports it.
//
// The shell contract is documented on RunShellAction (hook_shell.hpp): JSON on
// stdin, exit 0 pass, exit 2 block, anything else fails open and logs.

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace minimal::config {

struct HookActionConfig {
    // The hook point this action answers, e.g. "core.pre_tool_use". A name no
    // registered point claims is a sanity-check warning, never a load error:
    // a plugin can be disabled while its point's actions stay in the file.
    std::string on;
    // field -> exact value, ANDed. Only fields the POINT declares filterable
    // are legal (validated against the registry, warned otherwise); values are
    // strings compared for exact equality against string payload fields. There
    // is deliberately no substring matching and no way to filter on
    // model-controlled input contents.
    std::vector<std::pair<std::string, std::string>> filter;
    // Run through /bin/sh -c with the JSON payload on stdin. Never
    // argv-interpolated: the payload is data, not a command line.
    std::string command;
    // Wall-clock budget for ONE invocation. nullopt -> default_timeout_ms.
    // 0 disables the budget (cancel still applies) -- the advisor's
    // advisor_timeout_ms precedent: a user may accept a wedged hook, never a
    // silent one. Negative values are refused at parse.
    std::optional<int> timeout_ms;
};

struct HooksConfig {
    int default_timeout_ms = 10000;
    std::vector<HookActionConfig> hooks;
};

// Strict (unknown fields included). Throws ConfigError naming the offending path when the
// document is not an object, `hooks` is not an array, an entry is missing
// `on`/`command`, `on` or `command` is not a non-empty string, a filter is not
// an object of string->string, or a timeout is negative. A missing file is NOT
// an error here -- LoadHooksConfig returns the default.
HooksConfig ParseHooksConfig(const nlohmann::json& doc);

// Thin: reads the file and parses it. Missing file -> default (no hooks);
// malformed file -> throws (the sanity check reports it, fatal like the other
// behavior-governing config).
HooksConfig LoadHooksConfig(const std::filesystem::path& path);

}  // namespace minimal::config
