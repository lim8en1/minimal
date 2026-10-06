#include "minimal/config/hooks_config.hpp"

#include <limits>
#include <string>

#include "minimal/common/config_reader.hpp"
#include "minimal/common/json_config_loader.hpp"

namespace minimal::config {

HooksConfig ParseHooksConfig(const nlohmann::json& doc) {
    constexpr std::int64_t kIntMax = std::numeric_limits<int>::max();
    HooksConfig config;
    Reader root(doc, "hooks.json");
    // 0 = no budget (a cancellable emission can still be abandoned; an
    // uncancellable one gets the hook shell's floor). Negative is refused:
    // silently disabling a safety budget is never the intent.
    config.default_timeout_ms =
        static_cast<int>(root.Int("default_timeout_ms", config.default_timeout_ms, 0, kIntMax));

    if (const auto* hooks = root.Raw("hooks")) {
        if (!hooks->is_array()) root.Fail("hooks", "must be a list of hook actions");
        for (std::size_t index = 0; index < hooks->size(); ++index) {
            const std::string segment = "hooks[" + std::to_string(index) + "]";
            const auto& node = (*hooks)[index];
            if (!node.is_object()) root.Fail(segment, "must be an object");
            Reader entry = root.Child(node, segment);
            HookActionConfig action;
            action.on = entry.RequireString("on");
            action.command = entry.RequireString("command");
            if (const auto timeout = entry.MaybeInt("timeout_ms", 0, kIntMax)) {
                action.timeout_ms = static_cast<int>(*timeout);
            }
            if (const auto filter = entry.MaybeStringMap("filter")) {
                for (const auto& [field, value] : *filter) {
                    if (field.empty()) entry.Fail("filter", "field names must be non-empty");
                    action.filter.emplace_back(field, value);
                }
            }
            entry.Finish();
            config.hooks.push_back(std::move(action));
        }
    }
    root.Finish();
    return config;
}

HooksConfig LoadHooksConfig(const std::filesystem::path& path) {
    // No hooks.json, or an empty one, is the normal no-hooks state -- the only
    // loader where an empty-but-present file is absence, not malformed content.
    return LoadJsonConfig<HooksConfig>(path, "hooks.json", AbsencePolicy::kDefaultOnMissing,
                                       EmptyFilePolicy::kTreatAsAbsent, ParseHooksConfig);
}

}  // namespace minimal::config
