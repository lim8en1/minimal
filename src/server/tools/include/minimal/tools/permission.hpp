#pragma once

#include <optional>
#include <string>
#include <unordered_map>

namespace minimal::tools {

enum class ToolCategory { ReadOnly, FileWrite, Execute };

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

    PermissionMode mode_for(const std::string& tool_name, ToolCategory category) const;
};

struct McpPermissionPolicy {
    std::unordered_map<std::string, PermissionMode> per_server;
    PermissionMode default_mode = PermissionMode::Ask;

    PermissionMode mode_for(const std::string& server_name) const;
};

}  // namespace minimal::tools
