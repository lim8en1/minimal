#include "memory_settings.hpp"

#include <algorithm>
#include <string>

#include "minimal/config/parse_util.hpp"

namespace minimal::memory {
namespace {

std::string StringOr(const nlohmann::json& config, const char* key, std::string fallback) {
    if (auto it = config.find(key); it != config.end() && it->is_string()) {
        return it->get<std::string>();
    }
    return fallback;
}

int IntOr(const nlohmann::json& config, const char* key, int fallback) {
    if (auto it = config.find(key); it != config.end() && it->is_number_integer()) {
        return it->get<int>();
    }
    return fallback;
}

double DoubleOr(const nlohmann::json& config, const char* key, double fallback) {
    if (auto it = config.find(key); it != config.end() && it->is_number()) {
        return it->get<double>();
    }
    return fallback;
}

bool BoolOr(const nlohmann::json& config, const char* key, bool fallback) {
    if (auto it = config.find(key); it != config.end() && it->is_boolean()) {
        return it->get<bool>();
    }
    return fallback;
}

// Ceiling clamps -- a misconfigured huge (or negative) value must not mean
// unbounded, nor wrap when cast to an unsigned size elsewhere.
int Clamp(int value, int lo, int hi) { return std::min(std::max(value, lo), hi); }

}  // namespace

MemorySettings ParseMemorySettings(const nlohmann::json& config) {
    MemorySettings out;
    if (!config.is_object()) return out;
    config::RejectUnknownKeys(
        config,
        {"strategy", "api_base", "api_key", "model", "timeout_ms", "sidecar_python_path",
         "sidecar_model", "sidecar_startup_timeout_ms", "sidecar_request_timeout_ms",
         "tagger_model", "tagger_max_tokens", "tagger_timeout_ms", "max_tags",
         "max_tags_per_entry", "tag_merge_similarity", "allow_inline", "max_text_bytes",
         "batch_size", "default_limit", "writes_prompt", "index_enabled", "index_budget_bytes",
         "index_in_subagents"},
        "memory: config");

    const auto strategy = StringOr(config, "strategy", "auto");
    if (strategy == "http") out.strategy = StrategyKind::kHttp;
    else if (strategy == "sidecar") out.strategy = StrategyKind::kSidecar;
    else if (strategy == "none") out.strategy = StrategyKind::kNone;
    else out.strategy = StrategyKind::kAuto;  // unknown value: auto, not a load failure

    out.api_base = StringOr(config, "api_base", "");
    out.api_key = StringOr(config, "api_key", "");
    out.model = StringOr(config, "model", "");
    out.timeout_ms = Clamp(IntOr(config, "timeout_ms", out.timeout_ms), 1, 600000);

    out.sidecar_python_path = StringOr(config, "sidecar_python_path", "");
    out.sidecar_model = StringOr(config, "sidecar_model", "");
    out.sidecar_startup_timeout_ms = Clamp(
        IntOr(config, "sidecar_startup_timeout_ms", out.sidecar_startup_timeout_ms), 1, 600000);
    out.sidecar_request_timeout_ms = Clamp(
        IntOr(config, "sidecar_request_timeout_ms", out.sidecar_request_timeout_ms), 1, 600000);

    out.tagger_model = StringOr(config, "tagger_model", "");
    out.tagger_max_tokens = Clamp(IntOr(config, "tagger_max_tokens", out.tagger_max_tokens), 64, 8192);
    out.tagger_timeout_ms = Clamp(IntOr(config, "tagger_timeout_ms", out.tagger_timeout_ms), 1000, 600000);
    out.max_tags = Clamp(IntOr(config, "max_tags", out.max_tags), 0, 1000);
    out.max_tags_per_entry = Clamp(IntOr(config, "max_tags_per_entry", out.max_tags_per_entry), 1, 16);
    out.tag_merge_similarity =
        std::min(std::max(DoubleOr(config, "tag_merge_similarity", out.tag_merge_similarity), 0.5), 1.0);

    out.allow_inline = BoolOr(config, "allow_inline", out.allow_inline);
    out.max_text_bytes = Clamp(IntOr(config, "max_text_bytes", out.max_text_bytes), 256, 1 << 20);
    out.batch_size = Clamp(IntOr(config, "batch_size", out.batch_size), 1, 256);
    out.default_limit = Clamp(IntOr(config, "default_limit", out.default_limit), 1, 64);
    out.writes_prompt = BoolOr(config, "writes_prompt", out.writes_prompt);

    out.index_enabled = BoolOr(config, "index_enabled", out.index_enabled);
    out.index_budget_bytes =
        Clamp(IntOr(config, "index_budget_bytes", out.index_budget_bytes), 512, 1 << 16);
    out.index_in_subagents = BoolOr(config, "index_in_subagents", out.index_in_subagents);
    return out;
}

}  // namespace minimal::memory
