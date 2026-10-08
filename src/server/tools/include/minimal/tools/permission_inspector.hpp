#pragma once

// What a plugin may ask of a session's permission gate (the /permissions
// command: explain, list and revoke grants), and the gate's verdict types --
// split out of tool_executor.hpp so the SDK names the contract, not the
// executor (ToolExecutor implements it; it stays core-internal).

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "minimal/tools/permission.hpp"
#include "minimal/tools/permission_model.hpp"
#include "minimal/tools/tool.hpp"

namespace minimal::tools {

// The gate's answer for one call, before any prompt.
struct PermissionVerdict {
    PermissionMode mode = PermissionMode::Ask;
    std::string reason;
    // False when an operator-written `ask` pattern (this policy's or the
    // ceiling's) matched: that Ask always reaches the checker, grants or not.
    bool ask_grantable = true;
    // The confinement classification the out-of-workdir fold used, for
    // Execute's re-check. checked=false when the fold never ran.
    bool confinement_checked = false;
    bool inside_project_root = false;
};

// A plugin's request to gate something that is not a tool call (an inbound
// signal, a resource read): see ToolExecutor::CheckSubject.
struct SubjectCheck {
    std::string owner;
    PermissionSubject subject;
    std::string summary;  // the prompt line
    PermissionMode default_mode = PermissionMode::Ask;
};
struct SubjectVerdict {
    bool allowed = false;
    std::string why;
};

class PermissionInspector {
public:
    virtual ~PermissionInspector() = default;

    // Explain by tool name, for the /permissions dry run: nullopt when no such
    // tool is registered. `grants_satisfy` (optional) reports whether the
    // grants held right now would answer a resulting Ask without a prompt.
    // SESSION WORKER ONLY: it reads the grant set calls mutate.
    virtual std::optional<PermissionVerdict> Explain(const std::string& tool_name,
                                                     const nlohmann::json& input,
                                                     const ToolContext& ctx,
                                                     bool* grants_satisfy = nullptr) const = 0;

    // The session's grants, sorted; and their revocation (one -- true if it
    // was held -- or all). SESSION WORKER ONLY.
    virtual std::vector<std::string> granted_keys() const = 0;
    virtual bool RevokeGrant(const std::string& key) = 0;
    virtual std::size_t RevokeAllGrants() = 0;

    virtual PermissionRunMode run_mode() const = 0;
    virtual const PermissionPolicy& policy() const = 0;
    virtual const PermissionModels& permission_models() const = 0;
};

}  // namespace minimal::tools
