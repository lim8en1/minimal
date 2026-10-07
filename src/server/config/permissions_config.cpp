#include "minimal/config/permissions_config.hpp"

#include <algorithm>
#include <chrono>
#include <limits>

#include "minimal/common/config_reader.hpp"
#include "minimal/common/json_config_loader.hpp"
#include "minimal/common/plugin_config_schema.hpp"
#include "minimal/config/permission_codec.hpp"
#include "minimal/common/clock.hpp"

namespace minimal::config {

namespace {

tools::PermissionMode ModeAt(Reader& reader, const std::string& key, const nlohmann::json& value) {
    if (!value.is_string()) {
        reader.Fail(key, "must be \"allow\", \"ask\" or \"deny\", got " + value.dump());
    }
    try {
        return ParsePermissionMode(value.get<std::string>());
    } catch (const std::exception& error) {
        reader.Fail(key, error.what());
    }
}

// One fallback category: allow | ask | deny, or unset (the harness default).
std::optional<tools::PermissionMode> MaybeMode(Reader& reader, const char* key,
                                               const std::string& what) {
    reader.Describe(key, {.doc = what + ". An agent's own rule for it wins. Unset: the harness "
                                        "default (read_only allow, the rest ask)."});
    const auto value = reader.MaybeEnum(key, tools::mode_names());
    if (!value) return std::nullopt;
    return tools::mode_from_name(*value);
}

void ParseFallback(Reader& root, tools::PermissionOverrideLayer& layer) {
    Reader fallback = root.Object("fallback");
    layer.read_only = MaybeMode(fallback, "read_only", "Tools that only read");
    layer.file_write = MaybeMode(fallback, "file_write", "Tools that write files");
    layer.execute = MaybeMode(fallback, "execute", "Tools that run programs");
    layer.out_of_workdir =
        MaybeMode(fallback, "out_of_workdir", "A tool call reaching outside the project directory");
    layer.network = MaybeMode(fallback, "network", "A tool call reaching the network");
    fallback.Finish();
}

// A {name: ...} map the form edits as JSON: Raw records it as such in
// describe mode; the returned cursor validates it as before.
Reader JsonMap(Reader& root, const char* key, const std::string& doc) {
    root.Describe(key, {.doc = doc});
    (void)root.Raw(key);
    return root.Object(key);
}

// {glob: mode} rules, in file order.
std::vector<tools::PermissionPatternRule> ParseRules(Reader& rules) {
    std::vector<tools::PermissionPatternRule> out;
    for (const auto& [glob, mode] : rules.node().items()) {
        if (IsCommentKey(glob)) continue;
        rules.Accept(glob);
        if (glob.empty()) rules.Fail("", "has an empty glob");
        out.push_back({glob, ModeAt(rules, glob, mode)});
    }
    return out;
}

}  // namespace

PermissionsConfig ParsePermissionsConfig(const nlohmann::json& doc) {
    PermissionsConfig config;
    Reader root(doc, "permissions.json");
    root.Describe("default_run_mode",
                  {.doc = "The run mode a new session starts in: normal asks, bypass approves "
                          "every prompt, allow-only denies every prompt. shift+tab changes it "
                          "for one session."});
    root.Default("default_run_mode", std::string(tools::run_mode_name(config.default_run_mode)));
    if (const auto mode = root.MaybeEnum("default_run_mode", tools::run_mode_names())) {
        config.default_run_mode = ParsePermissionRunMode(*mode);
    }
    ParseFallback(root, config.global_layer);

    Reader per_tool = JsonMap(root, "per_tool_overrides",
                              "{tool: allow|ask|deny} for one tool, over its category's fallback "
                              "(MCP tools: mcp__<server>__<tool>).");
    for (const auto& [key, value] : per_tool.node().items()) {
        if (IsCommentKey(key)) continue;
        per_tool.Accept(key);
        if (key.empty()) per_tool.Fail("", "has an empty-string key");
        config.global_layer.per_tool_overrides[key] = ModeAt(per_tool, key, value);
    }

    Reader patterns = JsonMap(root, "pattern_overrides",
                              "{tool: {glob: allow|ask|deny}}: rules on one invocation's subject "
                              "-- a bash command line, a path (\"bash\": {\"git status*\": "
                              "\"allow\"}).");
    for (const auto& [tool, unused] : patterns.node().items()) {
        (void)unused;
        if (IsCommentKey(tool)) continue;
        if (tool.empty()) patterns.Fail("", "has an empty-string key");
        auto tool_rules = patterns.MaybeObject(tool);
        config.global_layer.pattern_overrides[tool] = ParseRules(*tool_rules);
    }

    Reader external = JsonMap(root, "external_directory_patterns",
                              "{glob: allow|ask|deny} for paths outside the project directory, "
                              "over fallback.out_of_workdir.");
    config.global_layer.out_of_workdir_patterns = ParseRules(external);

    root.Describe("pattern_precedence",
                  {.doc = "How matching pattern rules combine: restrictive (the strictest "
                          "wins) or specific (a deny still wins; otherwise the most specific "
                          "rule does, so \"git status\": allow carves out of \"git *\": ask). "
                          "Unset: restrictive."});
    if (const auto precedence = root.MaybeEnum("pattern_precedence", tools::precedence_names())) {
        config.global_layer.pattern_precedence = ParsePatternPrecedence(*precedence);
    }
    // Each plugin permission model's slice, verbatim: the model owns its shape,
    // and its form is the plugin's own (MaybeObject records no field).
    if (auto slices = root.MaybeObject("plugins")) config.plugin_slices = slices->node();
    root.Describe("advisor",
                  {.doc = "The plugin whose advisor answers prompts before you do; whatever it "
                          "is unsure of still reaches you. Unset: every prompt reaches you.",
                   .empty_label = "no advisor: every prompt reaches you"});
    config.advisor = root.MaybeString("advisor", /*allow_empty=*/false);
    root.Describe("advisor_inherit",
                  {.doc = "Subagents use the advisor too. Off: a subagent's prompts reach you "
                          "even when its parent is advised."});
    config.advisor_inherit = root.Bool("advisor_inherit", config.advisor_inherit);
    root.Describe("advisor_timeout_ms",
                  {.doc = "How long the advisor gets to answer one prompt before you are asked "
                          "instead. 0: no limit beyond the advisor's own."});
    config.advisor_timeout_ms = static_cast<int>(
        root.Int("advisor_timeout_ms", config.advisor_timeout_ms, 0,
                 std::min<std::int64_t>(std::numeric_limits<int>::max(),
                                        common::kMaxDeadline.count())));
    root.Finish();
    return config;
}

nlohmann::json DescribePermissionsConfig() {
    return DescribeConfig("permissions.json",
                          [](const nlohmann::json& doc) { (void)ParsePermissionsConfig(doc); });
}

std::string ValidatePermissionsConfig(const nlohmann::json& doc) {
    return ValidateConfig([](const nlohmann::json& d) { (void)ParsePermissionsConfig(d); }, doc);
}

PermissionsConfig LoadPermissionsConfig(const std::filesystem::path& path) {
    return LoadJsonConfig<PermissionsConfig>(path, "permissions.json", AbsencePolicy::kThrow,
                                             EmptyFilePolicy::kTreatAsMalformed,
                                             ParsePermissionsConfig);
}

}  // namespace minimal::config
