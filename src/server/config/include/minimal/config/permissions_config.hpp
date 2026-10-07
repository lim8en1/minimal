#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "minimal/tools/permission.hpp"
#include "minimal/config/policy.hpp"

namespace minimal::config {

struct PermissionsConfig {
    tools::PermissionRunMode default_run_mode = tools::PermissionRunMode::Normal;
    // fallback.{read_only,file_write,execute,out_of_workdir} and the top-level
    // per_tool_overrides key both land here -- PermissionOverrideLayer already
    // has both shapes, so reusing it avoids a redundant struct even though the
    // two are JSON siblings, not nested, in permissions.json itself.
    tools::PermissionOverrideLayer global_layer;
    // `plugins`: {<model>: {...}} -- each plugin permission model's global
    // slice, verbatim. Core checks only the outer shape; the model's
    // BuildLayer reads inside (tools/permission_model.hpp).
    nlohmann::json plugin_slices = nlohmann::json::object();
    // The plugin name whose registered permission advisor answers Ask-gated
    // tool calls automatically (abstaining to the human). The first
    // plugin-name field in permissions.json -- the startup sanity check
    // resolves it against loaded plugins and warns when it names nothing.
    // Unset means no advisor: every Ask reaches the human as today.
    std::optional<std::string> advisor;
    // Subagents default to NO advisor -- an auto-approver silently governing
    // children violates the spirit of "a child is never more permissive than
    // its parent" even though the ceilings technically hold. Opting in copies
    // the parent's advisor to the child at spawn; the ceilings still bound it.
    bool advisor_inherit = false;
    // The budget an advisor gets to answer one Ask before the composition
    // abandons it and falls through to the human. An external decider (an LLM
    // judge, a remote policy service) can hang; without a budget a wedged
    // advisor wedges the session worker -- and through it Terminate, the
    // reaper and process shutdown. 0 disables the budget (Advise is then
    // bounded only by its cancellation contract). Negative values are refused
    // at parse -- silently disabling a safety timeout is never intended.
    int advisor_timeout_ms = static_cast<int>(kDefaultAdvisorTimeout.count());
};

// Pure and strict: an unknown key, a wrong type or an invalid mode is a
// ConfigError naming its path (a typo'd permission rule silently ignored is
// less protection than written). An empty-string key inside
// per_tool_overrides is refused -- reserved as the category-only-resolution
// sentinel used by agent::BuildPermissionPolicy.
PermissionsConfig ParsePermissionsConfig(const nlohmann::json& doc);

// The file's schema (config::DescribeConfig over the parser above) -- for
// /config's permissions.json form. Defined beside the parser on purpose:
// describe mode is a thread-local in config_reader, and a caller in another
// binary (the server executable, a test) holds its own copy of it.
nlohmann::json DescribePermissionsConfig();
// "" when ParsePermissionsConfig accepts `doc`, else its message.
std::string ValidatePermissionsConfig(const nlohmann::json& doc);

// Thin: reads the file and parses it. Throws if the file is missing or isn't
// valid JSON.
PermissionsConfig LoadPermissionsConfig(const std::filesystem::path& path);

}  // namespace minimal::config
