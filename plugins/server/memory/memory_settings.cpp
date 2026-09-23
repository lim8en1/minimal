#include "memory_settings.hpp"

#include <string>

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

bool BoolOr(const nlohmann::json& config, const char* key, bool fallback) {
    if (auto it = config.find(key); it != config.end() && it->is_boolean()) {
        return it->get<bool>();
    }
    return fallback;
}

}  // namespace

MemorySettings ParseMemorySettings(const nlohmann::json& config) {
    MemorySettings out;

    const auto strategy = StringOr(config, "strategy", "auto");
    if (strategy == "http") out.strategy = StrategyKind::kHttp;
    else if (strategy == "sidecar") out.strategy = StrategyKind::kSidecar;
    else if (strategy == "none") out.strategy = StrategyKind::kNone;
    else out.strategy = StrategyKind::kAuto;  // unknown value: auto, not a load failure

    out.api_base = StringOr(config, "api_base", "");
    out.api_key_env = StringOr(config, "api_key_env", "");
    out.model = StringOr(config, "model", "");
    out.timeout_ms = IntOr(config, "timeout_ms", out.timeout_ms);

    out.sidecar_python_path = StringOr(config, "sidecar_python_path", "");
    out.sidecar_model = StringOr(config, "sidecar_model", "");
    out.sidecar_startup_timeout_ms =
        IntOr(config, "sidecar_startup_timeout_ms", out.sidecar_startup_timeout_ms);
    out.sidecar_request_timeout_ms =
        IntOr(config, "sidecar_request_timeout_ms", out.sidecar_request_timeout_ms);

    out.summarizer_model = StringOr(config, "summarizer_model", "");
    out.summarize_max_tokens = IntOr(config, "summarize_max_tokens", out.summarize_max_tokens);
    out.summarize_input_char_budget =
        IntOr(config, "summarize_input_char_budget", out.summarize_input_char_budget);
    out.max_topics = IntOr(config, "max_topics", out.max_topics);

    out.allow_inline = BoolOr(config, "allow_inline", out.allow_inline);
    out.max_text_bytes = IntOr(config, "max_text_bytes", out.max_text_bytes);
    out.batch_size = IntOr(config, "batch_size", out.batch_size);
    out.reindex_budget_rows = IntOr(config, "reindex_budget_rows", out.reindex_budget_rows);
    out.default_limit = IntOr(config, "default_limit", out.default_limit);
    out.digest_enabled = BoolOr(config, "digest_enabled", out.digest_enabled);

    // Ceiling clamps -- a misconfigured huge value must not mean unbounded.
    if (out.batch_size < 1) out.batch_size = 1;
    if (out.batch_size > 256) out.batch_size = 256;
    if (out.default_limit < 1) out.default_limit = 1;
    if (out.default_limit > 64) out.default_limit = 64;
    if (out.max_topics < 0) out.max_topics = 0;
    if (out.max_text_bytes < 256) out.max_text_bytes = 256;
    if (out.max_text_bytes > 1 << 20) out.max_text_bytes = 1 << 20;
    // reindex_budget_rows feeds `LIMIT ?1` in ClaimPendingEmbeddings, where
    // SQLite reads a negative limit as "no limit" -- -1 (or anything
    // negative) silently turned one lazy drain into a full-table scan.
    if (out.reindex_budget_rows < 0) out.reindex_budget_rows = 0;
    if (out.reindex_budget_rows > 10000) out.reindex_budget_rows = 10000;
    // The three timeouts and summarizer budgets were parsed with no ceiling
    // at all; each is a plain positive setting with no reason to allow
    // billions.
    if (out.timeout_ms < 0) out.timeout_ms = 0;
    if (out.timeout_ms > 600000) out.timeout_ms = 600000;
    if (out.sidecar_startup_timeout_ms < 0) out.sidecar_startup_timeout_ms = 0;
    if (out.sidecar_startup_timeout_ms > 600000) out.sidecar_startup_timeout_ms = 600000;
    if (out.sidecar_request_timeout_ms < 0) out.sidecar_request_timeout_ms = 0;
    if (out.sidecar_request_timeout_ms > 600000) out.sidecar_request_timeout_ms = 600000;
    if (out.summarize_max_tokens < 1) out.summarize_max_tokens = 1;
    if (out.summarize_max_tokens > 4096) out.summarize_max_tokens = 4096;
    // A NEGATIVE budget cast to size_t elsewhere would wrap to a huge
    // unsigned value and disable truncation entirely.
    if (out.summarize_input_char_budget < 0) out.summarize_input_char_budget = 0;
    if (out.summarize_input_char_budget > 1 << 20) out.summarize_input_char_budget = 1 << 20;
    return out;
}

}  // namespace minimal::memory
