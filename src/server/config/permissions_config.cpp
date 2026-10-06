#include "minimal/config/permissions_config.hpp"

#include <chrono>

#include "minimal/common/config_reader.hpp"
#include "minimal/common/json_config_loader.hpp"
#include "minimal/config/permission_codec.hpp"

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

std::optional<tools::PermissionMode> MaybeMode(Reader& reader, const char* key) {
    const auto* value = reader.Raw(key);
    if (value == nullptr) return std::nullopt;
    return ModeAt(reader, key, *value);
}

void ParseFallback(Reader& root, tools::PermissionOverrideLayer& layer) {
    Reader fallback = root.Object("fallback");
    layer.read_only = MaybeMode(fallback, "read_only");
    layer.file_write = MaybeMode(fallback, "file_write");
    layer.execute = MaybeMode(fallback, "execute");
    layer.out_of_workdir = MaybeMode(fallback, "out_of_workdir");
    layer.network = MaybeMode(fallback, "network");
    fallback.Finish();
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
    if (const auto mode = root.MaybeString("default_run_mode")) {
        try {
            config.default_run_mode = ParsePermissionRunMode(*mode);
        } catch (const std::exception& error) {
            root.Fail("default_run_mode", error.what());
        }
    }
    ParseFallback(root, config.global_layer);

    Reader per_tool = root.Object("per_tool_overrides");
    for (const auto& [key, value] : per_tool.node().items()) {
        if (IsCommentKey(key)) continue;
        per_tool.Accept(key);
        if (key.empty()) per_tool.Fail("", "has an empty-string key");
        config.global_layer.per_tool_overrides[key] = ModeAt(per_tool, key, value);
    }

    Reader patterns = root.Object("pattern_overrides");
    for (const auto& [tool, unused] : patterns.node().items()) {
        (void)unused;
        if (IsCommentKey(tool)) continue;
        if (tool.empty()) patterns.Fail("", "has an empty-string key");
        auto tool_rules = patterns.MaybeObject(tool);
        config.global_layer.pattern_overrides[tool] = ParseRules(*tool_rules);
    }

    Reader external = root.Object("external_directory_patterns");
    config.global_layer.out_of_workdir_patterns = ParseRules(external);

    if (const auto precedence = root.MaybeString("pattern_precedence")) {
        try {
            config.global_layer.pattern_precedence = ParsePatternPrecedence(*precedence);
        } catch (const std::exception& error) {
            root.Fail("pattern_precedence", error.what());
        }
    }
    // Each plugin permission model's slice, verbatim: the model owns its shape.
    if (auto slices = root.MaybeObject("plugins")) config.plugin_slices = slices->node();
    config.advisor = root.MaybeString("advisor", /*allow_empty=*/false);
    config.advisor_inherit = root.Bool("advisor_inherit", config.advisor_inherit);
    config.advisor_timeout_ms = static_cast<int>(
        root.Int("advisor_timeout_ms", config.advisor_timeout_ms, 0,
                 std::chrono::milliseconds(std::chrono::hours(24)).count()));
    root.Finish();
    return config;
}

PermissionsConfig LoadPermissionsConfig(const std::filesystem::path& path) {
    return LoadJsonConfig<PermissionsConfig>(path, "permissions.json", AbsencePolicy::kThrow,
                                             EmptyFilePolicy::kTreatAsMalformed,
                                             ParsePermissionsConfig);
}

}  // namespace minimal::config
