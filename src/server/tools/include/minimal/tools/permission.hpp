#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace minimal::tools {

enum class ToolCategory { ReadOnly, FileWrite, Execute };

// --- Consent scoping ---------------------------------------------------------
//
// A tool may disclose what an invocation will actually reach, so an `Ask` prompt
// can offer session-scoped grants ("allow /usr/bin/make * this session") instead
// of asking about every invocation. This is a prompt-fatigue mechanism, NOT a
// security boundary: a grant is blanket permission for that key, and for bash a
// disclosure is a lower bound (`find . | xargs rm` names find and xargs, never
// rm). Nothing here constrains what a tool can do -- the permission mode does.
struct GrantOffer {
    std::string key;    // stable, e.g. "bash:/usr/bin/make" -- what gets remembered
    std::string label;  // human-facing, e.g. "/usr/bin/make *"
};

struct PermissionDisclosure {
    std::string summary;             // one line for the prompt
    std::vector<GrantOffer> grants;  // what could be named; may be a lower bound
    // False when the invocation cannot be named at all (bash: `eval "$X"`, `$X
    // args`). Such a disclosure can never satisfy the grant conjunction -- see
    // ToolExecutor::SatisfiedByGrants for why that is a logic requirement rather
    // than a policy judgement about which programs are dangerous.
    bool complete = true;
};

// Ordered: Deny < Ask < Allow. The ordering is the point -- most_restrictive() is
// the lattice meet, and several composition rules depend on it.
enum class PermissionMode { AlwaysDeny = 0, Ask = 1, AlwaysAllow = 2 };

// allow+ask = ask, ask+deny = deny, deny+allow = deny.
constexpr PermissionMode most_restrictive(PermissionMode a, PermissionMode b) {
    return (static_cast<int>(a) < static_cast<int>(b)) ? a : b;
}

// Least to most permissive.
enum class PermissionRunMode { AllowOnly, Normal, Bypass };

// Bypass    -> AlwaysAllow, ignoring `base` entirely.
// AllowOnly -> base's Allow/Deny pass through; base's Ask becomes Deny, so a
//              non-interactive run auto-denies instead of blocking forever on a
//              human who was never attached.
// Normal    -> base unchanged.
constexpr PermissionMode apply_run_mode(PermissionMode base, PermissionRunMode run_mode) {
    switch (run_mode) {
        case PermissionRunMode::Bypass:
            return PermissionMode::AlwaysAllow;
        case PermissionRunMode::AllowOnly:
            return base == PermissionMode::Ask ? PermissionMode::AlwaysDeny : base;
        case PermissionRunMode::Normal:
            break;
    }
    return base;
}

// A child session may never be more permissive than its parent.
constexpr PermissionRunMode clamp_run_mode(PermissionRunMode parent, PermissionRunMode child) {
    return (static_cast<int>(child) < static_cast<int>(parent)) ? child : parent;
}

struct PermissionPolicy {
    PermissionMode read_only = PermissionMode::AlwaysAllow;
    PermissionMode file_write = PermissionMode::Ask;
    PermissionMode execute = PermissionMode::Ask;
    // A second dimension, not a category: folded in via most_restrictive() only
    // when a path-bearing tool resolves outside project_root. Substituting it for
    // the tool's category would invert the intent -- `file_write: deny` plus
    // `out_of_workdir: allow` would permit /etc/foo while denying ./foo.cpp.
    PermissionMode out_of_workdir = PermissionMode::Ask;

    std::unordered_map<std::string, PermissionMode> per_tool_overrides;

    PermissionMode mode_for(std::string_view tool_name, ToolCategory category) const;
};

struct McpPermissionPolicy {
    std::unordered_map<std::string, PermissionMode> per_server;
    PermissionMode default_mode = PermissionMode::Ask;

    PermissionMode mode_for(std::string_view server_name) const;
};

// --- Permission precedence chains (Config -- "Permission-mode precedence") ---
//
// Pure functions over already-parsed config layers -- no file/YAML reading here.
// Unlike PermissionPolicy (whose fields are always concrete), a layer may leave
// any field unset, meaning "fall through to the next layer".
struct PermissionOverrideLayer {
    std::optional<PermissionMode> read_only, file_write, execute, out_of_workdir;
    std::unordered_map<std::string, PermissionMode> per_tool_overrides;
};

// preset per-tool override -> preset category -> global per_tool_overrides ->
// global fallback.<category> -> hard-coded default (read_only=Allow, others=Ask).
PermissionMode resolve_permission_chain(const std::string& tool_name, ToolCategory category,
                                         const PermissionOverrideLayer& preset,
                                         const PermissionOverrideLayer& global);

// out_of_workdir is a dimension, not a ToolCategory (see PermissionPolicy above),
// so it gets its own two-layer chain rather than flowing through mode_for().
// preset.out_of_workdir -> global.out_of_workdir -> hard-coded default (Ask).
PermissionMode resolve_out_of_workdir_chain(const PermissionOverrideLayer& preset,
                                             const PermissionOverrideLayer& global);

// preset mcp_permissions[server] -> permissions.json mcp_default_mode -> Ask.
PermissionMode resolve_mcp_chain(const std::optional<PermissionMode>& preset_per_server,
                                  PermissionMode global_default_mode);

}  // namespace minimal::tools
