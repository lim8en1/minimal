#include "minimal/tools/permission.hpp"

#include <algorithm>

#include "minimal/tools/pattern_match.hpp"

namespace minimal::tools {

std::string_view mode_name(PermissionMode mode) {
    switch (mode) {
        case PermissionMode::AlwaysAllow: return "allow";
        case PermissionMode::Ask:         return "ask";
        case PermissionMode::AlwaysDeny:  return "deny";
    }
    return "ask";
}

std::string_view category_name(ToolCategory category) {
    switch (category) {
        case ToolCategory::ReadOnly:  return "read_only";
        case ToolCategory::FileWrite: return "file_write";
        case ToolCategory::Execute:   return "execute";
    }
    return "read_only";
}

namespace {

// The pattern half of mode_for: most_restrictive over every MATCHING rule for
// this key. nullopt = no match (the caller keeps the category default).
// `glob` (optional) receives the rule that decided -- the first rule holding
// the folded mode -- for the explanation.
std::optional<PermissionMode> FoldPatterns(const std::vector<PermissionPatternRule>& rules,
                                           const std::string& subject, bool slash_terminates,
                                           std::string* glob = nullptr) {
    std::optional<PermissionMode> folded;
    for (const auto& rule : rules) {
        if (GlobMatch(rule.glob, subject, slash_terminates)) {
            if (!folded || rule.mode < *folded) {
                folded = rule.mode;
                if (glob != nullptr) *glob = rule.glob;
            }
        }
    }
    return folded;
}

std::optional<PermissionMode> layer_category_mode(const PermissionOverrideLayer& layer,
                                                   ToolCategory category) {
    switch (category) {
        case ToolCategory::ReadOnly:  return layer.read_only;
        case ToolCategory::FileWrite: return layer.file_write;
        case ToolCategory::Execute:   return layer.execute;
    }
    return std::nullopt;  // unreachable
}

std::optional<PermissionMode> layer_tool_mode(const PermissionOverrideLayer& layer,
                                               std::string_view tool_name) {
    if (auto it = layer.per_tool_overrides.find(std::string(tool_name));
        it != layer.per_tool_overrides.end()) {
        return it->second;
    }
    return std::nullopt;
}

PermissionMode hardcoded_category_default(ToolCategory category) {
    static const PermissionPolicy defaults{};
    switch (category) {
        case ToolCategory::ReadOnly:  return defaults.read_only;
        case ToolCategory::FileWrite: return defaults.file_write;
        case ToolCategory::Execute:   return defaults.execute;
    }
    return PermissionMode::Ask;  // unreachable; fail closed
}

std::optional<PermissionMode> fold_optional(std::optional<PermissionMode> a,
                                            std::optional<PermissionMode> b) {
    if (a && b) return most_restrictive(*a, *b);
    return a ? a : b;
}

// Literal characters in a glob: what "more specific" means. An escaped
// character counts as one literal; wildcards count as nothing.
std::size_t Specificity(std::string_view glob) {
    std::size_t literal = 0;
    for (std::size_t i = 0; i < glob.size(); ++i) {
        if (glob[i] == '\\' && i + 1 < glob.size()) {
            ++literal;
            ++i;
        } else if (glob[i] != '*' && glob[i] != '?') {
            ++literal;
        }
    }
    return literal;
}

// The Specific-precedence winner among the rules matching `subject`: any
// matching deny wins; otherwise the most specific match (ties: stricter).
std::optional<PermissionMode> WinSpecific(const std::vector<PermissionPatternRule>& rules,
                                          const std::string& subject, bool slash_terminates,
                                          std::string* glob) {
    const PermissionPatternRule* best = nullptr;
    for (const auto& rule : rules) {
        if (!GlobMatch(rule.glob, subject, slash_terminates)) continue;
        if (best == nullptr) {
            best = &rule;
            continue;
        }
        const bool best_denies = best->mode == PermissionMode::AlwaysDeny;
        const bool rule_denies = rule.mode == PermissionMode::AlwaysDeny;
        if (best_denies != rule_denies) {
            if (rule_denies) best = &rule;
            continue;
        }
        const auto rule_score = Specificity(rule.glob);
        const auto best_score = Specificity(best->glob);
        if (rule_score > best_score || (rule_score == best_score && rule.mode < best->mode)) {
            best = &rule;
        }
    }
    if (best == nullptr) return std::nullopt;
    if (glob != nullptr) *glob = best->glob;
    return best->mode;
}

// "<where>: <key>=<mode>" -- the one shape every rule is named in.
std::string rule_text(std::string_view where, std::string_view key, PermissionMode mode) {
    std::string out(where);
    out += ": ";
    out += key;
    out += '=';
    out += mode_name(mode);
    return out;
}

}  // namespace

PermissionPolicy::ToolResolution PermissionPolicy::resolve_tool(std::string_view tool_name,
                                                                ToolCategory category) const {
    const std::string_view cat = category_name(category);
    if (!layers) {
        // Flat (directly constructed) policy: the exact entry, else the category.
        std::optional<PermissionMode> exact;
        if (auto it = per_tool_overrides.find(std::string(tool_name));
            it != per_tool_overrides.end()) {
            exact = it->second;
        }
        PermissionMode base = PermissionMode::Ask;  // fail closed
        switch (category) {
            case ToolCategory::ReadOnly:  base = read_only; break;
            case ToolCategory::FileWrite: base = file_write; break;
            case ToolCategory::Execute:   base = execute; break;
        }
        const PermissionMode mode = exact.value_or(base);
        return {mode, exact, std::nullopt,
                exact ? rule_text("per-tool rule", tool_name, mode)
                      : rule_text("category", cat, mode)};
    }

    const auto& preset = layers->preset;
    const auto& global = layers->global;
    const auto global_tool = layer_tool_mode(global, tool_name);
    const auto global_category = layer_category_mode(global, category);
    const PermissionMode global_only =
        global_tool.value_or(global_category.value_or(hardcoded_category_default(category)));
    const std::string global_only_text =
        global_tool      ? rule_text("permissions.json", tool_name, *global_tool)
        : global_category ? rule_text("permissions.json fallback", cat, *global_category)
                          : rule_text("default", cat, global_only);

    const auto preset_tool = layer_tool_mode(preset, tool_name);
    const auto preset_category = layer_category_mode(preset, category);

    if (!layers->preset_trusted) {
        // Resolve as if the preset were absent, then let the preset's own
        // opinion (its per-tool rule, else its category) only narrow.
        const auto clamp = preset_tool ? preset_tool : preset_category;
        std::string source = global_only_text;
        if (clamp && *clamp < global_only) {
            source = preset_tool ? rule_text("project preset", tool_name, *clamp)
                                 : rule_text("project preset", cat, *clamp);
        }
        return {global_only, global_tool, clamp, std::move(source)};
    }
    if (preset_tool) {
        return {*preset_tool, preset_tool, std::nullopt,
                rule_text("preset", tool_name, *preset_tool)};
    }
    // Neither a blanket preset category nor a specific global rule may widen
    // the other: when both speak they fold.
    if (preset_category && global_tool) {
        const PermissionMode mode = most_restrictive(*preset_category, *global_tool);
        return {mode, global_tool, std::nullopt,
                *preset_category <= *global_tool
                    ? rule_text("preset", cat, *preset_category) + " (stricter than " +
                          rule_text("permissions.json", tool_name, *global_tool) + ")"
                    : rule_text("permissions.json", tool_name, *global_tool) +
                          " (stricter than " + rule_text("preset", cat, *preset_category) + ")"};
    }
    if (preset_category) {
        return {*preset_category, global_tool, std::nullopt,
                rule_text("preset", cat, *preset_category)};
    }
    return {global_only, global_tool, std::nullopt, global_only_text};
}

PermissionMode PermissionPolicy::mode_for(std::string_view tool_name, ToolCategory category) const {
    const auto r = resolve_tool(tool_name, category);
    return r.clamp ? most_restrictive(r.base, *r.clamp) : r.base;
}

PermissionMode PermissionPolicy::mode_for(std::string_view tool_name, ToolCategory category,
                                          const std::optional<std::string>& subject,
                                          bool subject_is_path) const {
    PatternSubjects subjects;
    subjects.whole = subject;
    return mode_for(tool_name, category, subjects, subject_is_path);
}

PermissionMode PermissionPolicy::mode_for(std::string_view tool_name, ToolCategory category,
                                          const PatternSubjects& subjects, bool subject_is_path,
                                          std::string* reason) const {
    const auto resolution = resolve_tool(tool_name, category);
    std::string why = resolution.source;
    // Every exit folds the untrusted preset's narrowing last.
    auto finish = [&](PermissionMode mode) {
        if (resolution.clamp && *resolution.clamp < mode) {
            mode = *resolution.clamp;
            why = rule_text("project preset", tool_name, mode);
        }
        if (reason != nullptr) *reason = std::move(why);
        return mode;
    };

    const auto rules_it = pattern_overrides.find(std::string(tool_name));
    if (rules_it == pattern_overrides.end() ||
        (!subjects.whole && subjects.stages.empty() && subjects.restrict_only.empty())) {
        return finish(resolution.base);
    }
    const std::vector<PermissionPatternRule>& rules = rules_it->second;

    // The pattern verdict, if any rule speaks for this invocation.
    std::optional<PermissionMode> pattern;
    std::string pattern_why;
    auto quote = [](std::string_view glob, PermissionMode mode) {
        return "pattern `" + std::string(glob) + "`=" + std::string(mode_name(mode));
    };
    // Restrictive-only spellings: ask/deny rules match them, allow rules
    // never do (they could only widen through them).
    auto fold_restrictive = [&](const std::string& subject) {
        std::string glob;
        if (auto mode = FoldPatterns(rules, subject, subject_is_path, &glob);
            mode && *mode != PermissionMode::AlwaysAllow) {
            if (!pattern || *mode < *pattern) {
                pattern = *mode;
                pattern_why = quote(glob, *mode);
            }
        }
    };
    if (pattern_precedence == PatternPrecedence::Specific) {
        // Each subject gets ONE winner (deny first, then most specific).
        // Combination: any deny -> deny; any ask -> ask; allow only when the
        // allow path is open (the whole subject for unstaged tools, every
        // stage for staged ones). The whole line of a compound command still
        // counts for ask/deny -- rules like `* | sh` are written against it.
        std::optional<PermissionMode> restrictive;
        std::string restrictive_why;
        auto note = [&](PermissionMode mode, const std::string& glob) {
            if (!restrictive || mode < *restrictive) {
                restrictive = mode;
                restrictive_why = quote(glob, mode);
            }
        };
        bool all_allowed = subjects.allow_eligible;
        std::string allow_why;
        auto judge = [&](const std::string& subject, bool can_allow) {
            std::string glob;
            const auto winner = WinSpecific(rules, subject, subject_is_path, &glob);
            if (!winner) {
                if (can_allow) all_allowed = false;
                return;
            }
            if (*winner == PermissionMode::AlwaysAllow) {
                if (can_allow) allow_why = quote(glob, *winner);
                return;
            }
            note(*winner, glob);
            if (can_allow) all_allowed = false;
        };
        if (subjects.stages.empty()) {
            if (subjects.whole) {
                judge(*subjects.whole, /*can_allow=*/true);
            } else {
                all_allowed = false;
            }
        } else {
            for (const auto& stage : subjects.stages) judge(stage, /*can_allow=*/true);
            if (subjects.whole && subjects.stages.size() > 1) judge(*subjects.whole, false);
            if (subjects.stages.size() > 1) allow_why = "every command matches an allow pattern";
        }
        for (const auto& spelling : subjects.restrict_only) fold_restrictive(spelling);
        if (restrictive && (!pattern || *restrictive < *pattern)) {
            pattern = restrictive;
            pattern_why = restrictive_why;
        }
        if (!pattern && all_allowed && !allow_why.empty()) {
            pattern = PermissionMode::AlwaysAllow;
            pattern_why = allow_why;
        }
    } else if (subjects.stages.empty() && subjects.allow_eligible) {
        // No staged view: every matching rule on the whole subject folds.
        if (subjects.whole) {
            std::string glob;
            pattern = FoldPatterns(rules, *subjects.whole, subject_is_path, &glob);
            if (pattern) pattern_why = quote(glob, *pattern);
        }
        for (const auto& spelling : subjects.restrict_only) fold_restrictive(spelling);
    } else {
        // The restrictive half: ask/deny rules match the whole line or ANY
        // stage. A rule the operator wrote against the raw line keeps its
        // meaning, and a per-stage deny (`"rm *": deny`) catches a stage
        // hiding mid-pipeline. Allow never folds here: its reach is the
        // conjunction below only -- a stage matching an allow rule must not
        // vouch for the stages that do not.
        if (subjects.whole) fold_restrictive(*subjects.whole);
        for (const auto& stage : subjects.stages) fold_restrictive(stage);
        for (const auto& spelling : subjects.restrict_only) fold_restrictive(spelling);

        // The permissive half: allow is a CONJUNCTION over stages -- every
        // stage must match some allow rule. The whole-line subject is not
        // consulted: an allow rule's reach is per program, never the
        // unstructured line. Skipped when the tool could not vouch for the
        // decomposition (allow_eligible = false: a stage writes a file, or
        // the parse is shaky), so `"git status": allow` never rides
        // `git status > f`.
        if (!pattern && subjects.allow_eligible && !subjects.stages.empty()) {
            const bool all_allowed =
                std::all_of(subjects.stages.begin(), subjects.stages.end(),
                            [&](const std::string& stage) {
                                const auto mode = FoldPatterns(rules, stage, subject_is_path);
                                return mode && *mode == PermissionMode::AlwaysAllow;
                            });
            if (all_allowed) {
                pattern = PermissionMode::AlwaysAllow;
                pattern_why = "every command matches an allow pattern";
            }
        }
    }

    if (!pattern) return finish(resolution.base);
    // A matching pattern replaces the category default but FOLDS with an
    // exact per-tool rule: a scalar `bash: allow` cannot defeat a matching
    // deny (the dotenv defaults), and a per-tool deny is a kill switch no
    // pattern allow lifts. This used to hold only on the unstaged path -- the
    // staged one returned before consulting the exact rule, so real bash ran
    // `git status` under `bash: deny` + `"git *": allow`.
    if (resolution.exact && *resolution.exact < *pattern) {
        why = rule_text("per-tool rule", tool_name, *resolution.exact) + " (over " +
              pattern_why + ")";
        return finish(*resolution.exact);
    }
    why = std::move(pattern_why);
    return finish(*pattern);
}

bool PermissionPolicy::ask_pattern_matches(std::string_view tool_name,
                                           const PatternSubjects& subjects,
                                           bool subject_is_path) const {
    const auto it = pattern_overrides.find(std::string(tool_name));
    if (it == pattern_overrides.end()) return false;
    auto matches = [&](const std::string& subject) {
        for (const auto& rule : it->second) {
            if (rule.mode == PermissionMode::Ask &&
                GlobMatch(rule.glob, subject, subject_is_path)) {
                return true;
            }
        }
        return false;
    };
    if (subjects.whole && matches(*subjects.whole)) return true;
    for (const auto& stage : subjects.stages) {
        if (matches(stage)) return true;
    }
    for (const auto& spelling : subjects.restrict_only) {
        if (matches(spelling)) return true;
    }
    return false;
}

PermissionMode McpPermissionPolicy::mode_for(std::string_view server_name) const {
    if (auto it = per_server.find(std::string(server_name)); it != per_server.end()) {
        return it->second;
    }
    return default_mode;
}

PermissionMode resolve_mcp_tool_mode(const PermissionPolicy& policy,
                                     const McpPermissionPolicy& mcp_policy,
                                     std::string_view server_name, std::string_view tool_name,
                                     std::string* reason) {
    const PermissionMode server_mode = mcp_policy.mode_for(server_name);
    // McpPermissionPolicy::per_server carries only the PRESET's entries (the
    // global side is default_mode), so its presence is "the preset spoke".
    const bool preset_spoke_for_server =
        mcp_policy.per_server.count(std::string(server_name)) != 0;
    const std::string server_text =
        rule_text(preset_spoke_for_server ? "preset mcp server" : "mcp default", server_name,
                  server_mode);
    auto answer = [&](PermissionMode mode, std::string why) {
        if (reason != nullptr) *reason = std::move(why);
        return mode;
    };

    if (!policy.layers) {
        if (auto it = policy.per_tool_overrides.find(std::string(tool_name));
            it != policy.per_tool_overrides.end()) {
            return answer(it->second, rule_text("per-tool rule", tool_name, it->second));
        }
        return answer(server_mode, server_text);
    }
    const auto global_tool = layer_tool_mode(policy.layers->global, tool_name);
    const auto preset_tool = layer_tool_mode(policy.layers->preset, tool_name);
    if (!policy.layers->preset_trusted) {
        // server_mode is already clamped by BuildMcpPermissionPolicy.
        PermissionMode mode = global_tool && !preset_spoke_for_server
                                  ? *global_tool
                                  : fold_optional(global_tool, server_mode).value();
        std::string why = mode == server_mode
                              ? server_text
                              : rule_text("permissions.json", tool_name, mode);
        if (preset_tool && *preset_tool < mode) {
            mode = *preset_tool;
            why = rule_text("project preset", tool_name, mode);
        }
        return answer(mode, std::move(why));
    }
    if (preset_tool) return answer(*preset_tool, rule_text("preset", tool_name, *preset_tool));
    if (global_tool) {
        if (preset_spoke_for_server && server_mode < *global_tool) {
            return answer(server_mode, server_text);
        }
        return answer(*global_tool, rule_text("permissions.json", tool_name, *global_tool));
    }
    return answer(server_mode, server_text);
}

PermissionMode resolve_permission_chain(const std::string& tool_name, ToolCategory category,
                                         const PermissionOverrideLayer& preset,
                                         const PermissionOverrideLayer& global) {
    // The one implementation: PermissionPolicy's layered resolution, trusted.
    PermissionPolicy policy;
    policy.layers = std::make_shared<const ToolModeLayers>(ToolModeLayers{preset, global, true});
    return policy.mode_for(tool_name, category);
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
