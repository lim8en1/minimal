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

// One glob pattern rule on one subject. The glob grammar and the two subject
// flavors (paths vs command lines) live in pattern_match.hpp.
struct PermissionPatternRule {
    std::string glob;
    PermissionMode mode = PermissionMode::Ask;
};

// The subject set an invocation presents to pattern rules: the tool's
// whole-line subject (`bash: git status | wc -l`) plus, when the tool can
// decompose it, its per-stage subjects (`git status`, `wc -l`). Default-
// constructible: the "no stages" state reproduces the pre-stage semantics
// exactly, which is what path tools and stage-free tests want.
struct PatternSubjects {
    std::optional<std::string> whole;
    std::vector<std::string> stages;  // empty = no staged matching
};

struct PermissionPolicy {
    PermissionMode read_only = PermissionMode::AlwaysAllow;
    PermissionMode file_write = PermissionMode::Ask;
    PermissionMode execute = PermissionMode::Ask;
    // A second dimension, not a category: folded in via most_restrictive() only
    // when a path-bearing tool resolves outside project_root. Substituting it for
    // the tool's category would invert the intent -- `file_write: deny` plus
    // `out_of_workdir: allow` would permit /etc/foo while denying ./foo.cpp.
    PermissionMode out_of_workdir = PermissionMode::Ask;

    // A third, independent dimension for the same reason out_of_workdir is one:
    // a tool that reaches the network (S3's web_fetch/web_search) is a
    // different risk from "runs a subprocess" or "writes a file", and an
    // operator who sets `execute: allow` for a sandboxed bash should not
    // thereby also allow fetching arbitrary URLs. Folded in via
    // most_restrictive() only for tools that declare
    // Tool::network_access_required().
    PermissionMode network = PermissionMode::Ask;

    std::unordered_map<std::string, PermissionMode> per_tool_overrides;

    // Pattern rules, keyed by PERMISSION key (never an alias -- the same rule
    // per_tool_overrides obeys). Semantics: an exact per_tool_overrides entry
    // and the most_restrictive MATCHING pattern for the tool's subject FOLD --
    // exact does not short-circuit past the pattern layer (a short-circuit let
    // an AlwaysAllow exact override silently defeat a built-in pattern denial,
    // e.g. the dotenv rule, for every input, since the pattern layer was never
    // even consulted). No pattern match -> the exact entry wins as before
    // (folding with nullopt is a no-op); any match -> most_restrictive of the
    // two, so a matching deny pattern is undefeatable by a scalar allow.
    // Patterns are operator intent at tool granularity, the same standing
    // per_tool_overrides has -- everything downstream (out_of_workdir, network,
    // ceilings, run modes) still folds on top, so a pattern can never bypass
    // the substrate. The one shape this folding cannot express: a strict
    // default pattern plus a more-specific ALLOW ("git *": "ask" + "git
    // status": "allow" stays ask) -- the remedy is to write the broad rule
    // narrower, not to layer an override on top.
    std::unordered_map<std::string, std::vector<PermissionPatternRule>> pattern_overrides;

    // Path patterns on the out_of_workdir dimension: when an invocation
    // resolves outside project_root, the most_restrictive matching pattern
    // replaces the scalar `out_of_workdir` for that invocation (no match ->
    // the scalar). Matched against the canonical absolute path, so these are
    // the one pattern set that is anchored at "/" by construction.
    std::vector<PermissionPatternRule> out_of_workdir_patterns;

    PermissionMode mode_for(std::string_view tool_name, ToolCategory category) const;
    // Subject-ful overload: `subject` is the tool's PatternSubject -- a
    // canonical path for confinement tools, the raw command line for bash.
    // nullopt = no subject for this input; patterns cannot match.
    // `subject_is_path` selects the glob flavor: paths keep '*' and '?'
    // within one component ('**' crosses); command lines are not paths, so
    // '/' is ordinary there. The caller knows which it handed over.
    PermissionMode mode_for(std::string_view tool_name, ToolCategory category,
                            const std::optional<std::string>& subject,
                            bool subject_is_path) const;
    // Staged overload. Matching per mode:
    //   Ask / Deny -- the MOST RESTRICTIVE rule matching the whole line OR any
    //     stage. Deny coverage only grows ("rm *" catches `ls | rm -rf /`).
    //   Allow -- requires stages, and EVERY stage must match some allow rule
    //     (the grant conjunction: a compound runs unprompted only when each
    //     named program is covered). An allow rule matching only the whole
    //     line no longer allows -- "git diff *" once allowed `git diff; make
    //     install`, because the subject was the raw line. No stages (or no
    //     allow match) -> the fold below decides as usual.
    PermissionMode mode_for(std::string_view tool_name, ToolCategory category,
                            const PatternSubjects& subjects, bool subject_is_path) const;
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
    std::optional<PermissionMode> read_only, file_write, execute, out_of_workdir, network;
    std::unordered_map<std::string, PermissionMode> per_tool_overrides;
    // Pattern rules per permission key; a layer may carry none.
    std::unordered_map<std::string, std::vector<PermissionPatternRule>> pattern_overrides;
    // Pattern rules on the out_of_workdir dimension.
    std::vector<PermissionPatternRule> out_of_workdir_patterns;
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

// Same two-layer shape as resolve_out_of_workdir_chain, for the `network`
// dimension. preset.network -> global.network -> hard-coded default (Ask).
PermissionMode resolve_network_chain(const PermissionOverrideLayer& preset,
                                      const PermissionOverrideLayer& global);

// preset mcp_permissions[server] -> permissions.json mcp_default_mode -> Ask.
PermissionMode resolve_mcp_chain(const std::optional<PermissionMode>& preset_per_server,
                                  PermissionMode global_default_mode);

// Merges the two config layers' pattern rules into one resolved set: the
// preset entry REPLACES the global entry for an identical (tool, glob) pair --
// the same preset-wins rule per_tool_overrides follows -- and distinct globs
// from both layers coexist (evaluation folds them most-restrictively at
// resolve time). Same shape for the out_of_workdir dimension's flat list.
void merge_pattern_layers(
    const PermissionOverrideLayer& preset, const PermissionOverrideLayer& global,
    std::unordered_map<std::string, std::vector<PermissionPatternRule>>& out_patterns,
    std::vector<PermissionPatternRule>& out_ood_patterns);

// The built-in default pattern set, consulted only for tools neither config
// layer gave patterns for: dotenv files are denied for every tool in
// `dotenv_protected_permission_keys` (Tool::default_dotenv_protected() --
// ToolRegistry::DotenvProtectedPermissionKeys() -- structural, not a
// hardcoded read/write/edit literal, so a renamed or third-party file tool
// that opts in is covered too). Living in the DEFAULT layer (not a hard
// floor) is what keeps it overridable -- config patterns replace it wholesale
// per tool.
std::unordered_map<std::string, std::vector<PermissionPatternRule>> default_pattern_rules(
    const std::vector<std::string>& dotenv_protected_permission_keys);

}  // namespace minimal::tools
