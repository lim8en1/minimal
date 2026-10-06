#include "memory_settings.hpp"

#include <string>

#include "minimal/common/config_reader.hpp"
#include "minimal/common/plugin_config_label.hpp"

namespace minimal::memory {

MemorySettings ParseMemorySettings(const nlohmann::json& config) {
    MemorySettings out;
    config::Reader reader(config, common::PluginConfigLabel(common::kPluginConfigDirName, "memory"));
    // Bounds: a huge (or negative) value must not mean unbounded, nor wrap
    // when cast to an unsigned size elsewhere -- out of range is an error.
    const auto int_in = [&](const char* key, int& target, int lo, int hi) {
        target = static_cast<int>(reader.Int(key, target, lo, hi));
    };

    const std::string strategy =
        reader.Enum("strategy", "auto", {"auto", "http", "sidecar", "none"});
    if (strategy == "http") out.strategy = StrategyKind::kHttp;
    else if (strategy == "sidecar") out.strategy = StrategyKind::kSidecar;
    else if (strategy == "none") out.strategy = StrategyKind::kNone;
    else out.strategy = StrategyKind::kAuto;

    out.api_base = reader.String("api_base", "");
    out.api_key = reader.String("api_key", "");
    out.model = reader.String("model", "");
    int_in("timeout_ms", out.timeout_ms, 1, 600000);

    out.sidecar_python_path = reader.String("sidecar_python_path", "");
    out.sidecar_model = reader.String("sidecar_model", "");
    int_in("sidecar_startup_timeout_ms", out.sidecar_startup_timeout_ms, 1, 600000);
    int_in("sidecar_request_timeout_ms", out.sidecar_request_timeout_ms, 1, 600000);

    out.tagger_model = reader.String("tagger_model", "");
    int_in("tagger_max_tokens", out.tagger_max_tokens, 64, 8192);
    int_in("tagger_timeout_ms", out.tagger_timeout_ms, 1000, 600000);
    int_in("max_tags", out.max_tags, 0, 1000);
    int_in("max_tags_per_entry", out.max_tags_per_entry, 1, 16);
    out.tag_merge_similarity =
        reader.Double("tag_merge_similarity", out.tag_merge_similarity, 0.5, 1.0);

    out.allow_inline = reader.Bool("allow_inline", out.allow_inline);
    int_in("max_text_bytes", out.max_text_bytes, 256, 1 << 20);
    int_in("batch_size", out.batch_size, 1, 256);
    int_in("default_limit", out.default_limit, 1, 64);
    out.writes_prompt = reader.Bool("writes_prompt", out.writes_prompt);

    out.index_enabled = reader.Bool("index_enabled", out.index_enabled);
    int_in("index_budget_bytes", out.index_budget_bytes, 512, 1 << 16);
    out.index_in_subagents = reader.Bool("index_in_subagents", out.index_in_subagents);
    reader.Finish();
    return out;
}

}  // namespace minimal::memory
