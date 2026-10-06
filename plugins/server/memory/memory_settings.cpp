#include "memory_settings.hpp"
#include <cstdint>
#include <limits>
#include <algorithm>

#include <string>

#include "minimal/common/config_reader.hpp"
#include "minimal/common/units.hpp"
#include "policy.hpp"
#include "minimal/common/clock.hpp"

namespace minimal::memory {

MemorySettings ParseMemorySettings(const nlohmann::json& config) {
    MemorySettings out;
    config::Reader reader(config, "plugins.json", "memory.config");
    // Bounds: a huge (or negative) value must not mean unbounded, nor wrap
    // when cast to an unsigned size elsewhere -- out of range is an error.
    // Ceilings are what the field's type -- or a deadline -- can hold.
    constexpr int kIntMax = std::numeric_limits<int>::max();
    const int kMaxTimeoutMs =
        static_cast<int>(std::min<std::int64_t>(kIntMax, common::kMaxDeadline.count()));
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
    int_in("timeout_ms", out.timeout_ms, 1, kMaxTimeoutMs);

    out.sidecar_python_path = reader.String("sidecar_python_path", "");
    out.sidecar_model = reader.String("sidecar_model", "");
    int_in("sidecar_startup_timeout_ms", out.sidecar_startup_timeout_ms, 1, kMaxTimeoutMs);
    int_in("sidecar_request_timeout_ms", out.sidecar_request_timeout_ms, 1, kMaxTimeoutMs);

    out.tagger_model = reader.String("tagger_model", "");
    int_in("tagger_max_tokens", out.tagger_max_tokens, 1, kIntMax);
    int_in("tagger_timeout_ms", out.tagger_timeout_ms, 1, kMaxTimeoutMs);
    int_in("max_tags", out.max_tags, 0, kIntMax);
    int_in("max_tags_per_entry", out.max_tags_per_entry, 1, kIntMax);
    out.tag_merge_similarity =
        reader.Double("tag_merge_similarity", out.tag_merge_similarity, kMinTagMergeSimilarity, 1.0);

    out.allow_inline = reader.Bool("allow_inline", out.allow_inline);
    int_in("max_text_bytes", out.max_text_bytes, kMinTextBytes, kIntMax);
    int_in("batch_size", out.batch_size, 1, kIntMax);
    int_in("default_limit", out.default_limit, 1, kSearchLimitCeiling);
    out.writes_prompt = reader.Bool("writes_prompt", out.writes_prompt);

    out.index_enabled = reader.Bool("index_enabled", out.index_enabled);
    int_in("index_budget_bytes", out.index_budget_bytes, kMinIndexBudgetBytes, kIntMax);
    out.index_in_subagents = reader.Bool("index_in_subagents", out.index_in_subagents);
    reader.Finish();
    return out;
}

}  // namespace minimal::memory
