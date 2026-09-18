#include "minimal/tools/permission.hpp"

#include "minimal/tools/pattern_match.hpp"

namespace minimal::tools {

namespace {

// The pattern half of mode_for: most_restrictive over every MATCHING rule for
// this key. nullopt = no match (the caller keeps the category default).
std::optional<PermissionMode> FoldPatterns(const std::vector<PermissionPatternRule>& rules,
                                           const std::string& subject, bool slash_terminates) {
    std::optional<PermissionMode> folded;
    for (const auto& rule : rules) {
        if (GlobMatch(rule.glob, subject, slash_terminates)) {
            folded = folded ? most_restrictive(*folded, rule.mode) : rule.mode;
        }
    }
    return folded;
}

}  // namespace

PermissionMode PermissionPolicy::mode_for(std::string_view tool_name, ToolCategory category) const {
    if (auto it = per_tool_overrides.find(std::string(tool_name)); it != per_tool_overrides.end()) {
        return it->second;
    }
    switch (category) {
        case ToolCategory::ReadOnly:  return read_only;
        case ToolCategory::FileWrite: return file_write;
        case ToolCategory::Execute:   return execute;
    }
    return PermissionMode::Ask;  // unreachable; fail closed rather than fall through
}

PermissionMode PermissionPolicy::mode_for(std::string_view tool_name, ToolCategory category,
                                          const std::optional<std::string>& subject,
                                          bool subject_is_path) const {
    // Exact overrides keep the highest precedence they have always had -- but
    // FOLDED against a matching pattern, never short-circuited past it. A
    // short-circuit let an AlwaysAllow exact override (native
    // permission_mode.overrides, or the opencode tools:{name:true} alias,
    // which is meant to be a plain availability toggle) silently defeat a
    // built-in pattern denial (default_pattern_rules' dotenv rule) for every
    // input, since the pattern layer was never even consulted. most_restrictive
    // keeps the documented "exact wins" behavior on the common no-pattern-match
    // path (folding with nullopt is a no-op) while making a matching deny
    // pattern undefeatable by a scalar allow.
    std::optional<PermissionMode> exact;
    if (auto it = per_tool_overrides.find(std::string(tool_name)); it != per_tool_overrides.end()) {
        exact = it->second;
    }
    std::optional<PermissionMode> pattern;
    if (subject) {
        if (auto it = pattern_overrides.find(std::string(tool_name));
            it != pattern_overrides.end()) {
            pattern = FoldPatterns(it->second, *subject, subject_is_path);
        }
    }
    if (exact && pattern) return most_restrictive(*exact, *pattern);
    if (exact) return *exact;
    if (pattern) return *pattern;
    return mode_for(tool_name, category);
}

PermissionMode PermissionPolicy::mode_for(std::string_view tool_name, ToolCategory category,
                                          const PatternSubjects& subjects,
                                          bool subject_is_path) const {
    if (subjects.stages.empty()) {
        // No staged view of this invocation: exactly the old behavior.
        return mode_for(tool_name, category, subjects.whole, subject_is_path);
    }

    // One lookup for the whole call: every stage plus the whole-line subject
    // consult the SAME tool's rule vector below, so the map lookup that used
    // to run once per stage (and again for the conjunction check) now runs
    // once regardless of pipeline length.
    const auto rules_it = pattern_overrides.find(std::string(tool_name));
    const std::vector<PermissionPatternRule>* rules =
        rules_it != pattern_overrides.end() ? &rules_it->second : nullptr;

    // The restrictive half: ask/deny rules match the whole line or ANY stage.
    // A rule the operator wrote against the raw line keeps its meaning, and a
    // per-stage deny (`"rm *": deny`) catches a stage hiding mid-pipeline.
    std::optional<PermissionMode> folded;
    auto fold = [&](const std::optional<std::string>& subject) {
        if (!subject || !rules) return;
        if (auto mode = FoldPatterns(*rules, *subject, subject_is_path);
            mode && *mode != PermissionMode::AlwaysAllow) {
            // Allow never folds here: its reach is the conjunction below
            // only. A stage matching an allow rule must not vouch for the
            // stages that do not -- folded allow would bypass the
            // all-stages-covered check and silently widen the policy.
            folded = folded ? most_restrictive(*folded, *mode) : *mode;
        }
    };
    fold(subjects.whole);
    for (const auto& stage : subjects.stages) fold(stage);

    // The permissive half: allow is a CONJUNCTION over stages. Every stage
    // must match some allow rule; any unmatched stage leaves the fold below in
    // charge. This is the one place the whole-line subject is NOT consulted --
    // an allow rule's reach is per program, never the unstructured line.
    if (rules) {
        bool all_allowed = true;
        for (const auto& stage : subjects.stages) {
            const auto mode = FoldPatterns(*rules, stage, subject_is_path);
            if (!mode || *mode != PermissionMode::AlwaysAllow) {
                all_allowed = false;
                break;
            }
        }
        if (all_allowed) {
            const PermissionMode allow = PermissionMode::AlwaysAllow;
            return folded ? most_restrictive(*folded, allow) : allow;
        }
    }

    if (folded) return *folded;
    return mode_for(tool_name, category);
}

PermissionMode McpPermissionPolicy::mode_for(std::string_view server_name) const {
    if (auto it = per_server.find(std::string(server_name)); it != per_server.end()) {
        return it->second;
    }
    return default_mode;
}

namespace {

std::optional<PermissionMode> layer_category_mode(const PermissionOverrideLayer& layer,
                                                   ToolCategory category) {
    switch (category) {
        case ToolCategory::ReadOnly:  return layer.read_only;
        case ToolCategory::FileWrite: return layer.file_write;
        case ToolCategory::Execute:   return layer.execute;
    }
    return std::nullopt;  // unreachable
}

}  // namespace

PermissionMode resolve_permission_chain(const std::string& tool_name, ToolCategory category,
                                         const PermissionOverrideLayer& preset,
                                         const PermissionOverrideLayer& global) {
    if (auto it = preset.per_tool_overrides.find(tool_name); it != preset.per_tool_overrides.end()) {
        return it->second;
    }
    if (auto mode = layer_category_mode(preset, category)) return *mode;
    if (auto it = global.per_tool_overrides.find(tool_name); it != global.per_tool_overrides.end()) {
        return it->second;
    }
    if (auto mode = layer_category_mode(global, category)) return *mode;

    static const PermissionPolicy hardcoded_default{};
    return hardcoded_default.mode_for(tool_name, category);
}

PermissionMode resolve_out_of_workdir_chain(const PermissionOverrideLayer& preset,
                                             const PermissionOverrideLayer& global) {
    if (preset.out_of_workdir) return *preset.out_of_workdir;
    if (global.out_of_workdir) return *global.out_of_workdir;
    return PermissionMode::Ask;
}

PermissionMode resolve_network_chain(const PermissionOverrideLayer& preset,
                                      const PermissionOverrideLayer& global) {
    if (preset.network) return *preset.network;
    if (global.network) return *global.network;
    return PermissionMode::Ask;
}

PermissionMode resolve_mcp_chain(const std::optional<PermissionMode>& preset_per_server,
                                  PermissionMode global_default_mode) {
    if (preset_per_server) return *preset_per_server;
    return global_default_mode;
}

void merge_pattern_layers(
    const PermissionOverrideLayer& preset, const PermissionOverrideLayer& global,
    std::unordered_map<std::string, std::vector<PermissionPatternRule>>& out_patterns,
    std::vector<PermissionPatternRule>& out_ood_patterns) {
    out_patterns = global.pattern_overrides;
    for (const auto& [key, rules] : preset.pattern_overrides) {
        auto& out = out_patterns[key];  // absent key: the preset's rules are it
        for (const auto& rule : rules) {
            // Same glob: the preset (trusted by the time it gets here) wins.
            bool replaced = false;
            for (auto& existing : out) {
                if (existing.glob == rule.glob) {
                    existing = rule;
                    replaced = true;
                    break;
                }
            }
            if (!replaced) out.push_back(rule);
        }
    }
    out_ood_patterns = global.out_of_workdir_patterns;
    for (const auto& rule : preset.out_of_workdir_patterns) {
        bool replaced = false;
        for (auto& existing : out_ood_patterns) {
            if (existing.glob == rule.glob) {
                existing = rule;
                replaced = true;
                break;
            }
        }
        if (!replaced) out_ood_patterns.push_back(rule);
    }
}

std::unordered_map<std::string, std::vector<PermissionPatternRule>> default_pattern_rules(
    const std::vector<std::string>& dotenv_protected_permission_keys) {
    // Tools that opted into Tool::default_dotenv_protected() deny dotenv
    // files by default; matched against the canonical absolute path, so
    // "**/" covers every depth including the project root.
    const PermissionPatternRule dotenv_exact{"**/.env", PermissionMode::AlwaysDeny};
    const PermissionPatternRule dotenv_suffix{"**/.env.*", PermissionMode::AlwaysDeny};
    std::unordered_map<std::string, std::vector<PermissionPatternRule>> rules;
    for (const auto& key : dotenv_protected_permission_keys) {
        rules[key] = {dotenv_exact, dotenv_suffix};
    }
    return rules;
}

}  // namespace minimal::tools
