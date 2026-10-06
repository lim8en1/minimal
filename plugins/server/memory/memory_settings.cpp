#include "memory_settings.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>

#include "minimal/common/clock.hpp"
#include "minimal/common/config_reader.hpp"
#include "minimal/common/plugin_config_label.hpp"
#include "minimal/common/units.hpp"
#include "policy.hpp"

namespace minimal::memory {

MemorySettings ParseMemorySettings(const nlohmann::json& config) {
    MemorySettings out;
    config::Reader reader(config, common::PluginConfigLabel(common::kPluginConfigDirName, "memory"));
    // Bounds: a huge (or negative) value must not mean unbounded, nor wrap
    // when cast to an unsigned size elsewhere -- out of range is an error.
    // Ceilings are what the field's type -- or a deadline -- can hold.
    constexpr int kIntMax = std::numeric_limits<int>::max();
    const int kMaxTimeoutMs =
        static_cast<int>(std::min<std::int64_t>(kIntMax, common::kMaxDeadline.count()));
    const auto int_in = [&](const char* key, int& target, int lo, int hi) {
        target = static_cast<int>(reader.Int(key, target, lo, hi));
    };

    // Restart: the embedding backend and its model (vectors already stored
    // were made by them), the tagger model (resolved once), and what shapes
    // the tools (allow_inline, writes_prompt). Live: the limits the tools,
    // the tagger and the context index read per use.
    // Relevance follows the modes: the http fields under strategy http/auto,
    // the sidecar fields under sidecar/auto (auto picks from whichever is
    // set), the tag fields only once a tagger_model is set.
    constexpr auto kLive = config::ApplyPolicy::kLive;
    reader.Describe("strategy", {"Embedding backend: auto (sidecar if sidecar_model is set, "
                                 "else http if api_base is set, else lexical), http, sidecar, "
                                 "none."});
    reader.Describe("api_base", {"OpenAI-compatible embeddings endpoint (http strategy)."});
    reader.Describe("api_key", {"Key reference for api_base.", config::ApplyPolicy::kRestart,
                                /*secret=*/true});
    reader.Describe("model", {"Embedding model at api_base. Changing it invalidates stored "
                              "vectors."});
    reader.Describe("timeout_ms", {"HTTP embedding request timeout."});
    reader.Describe("sidecar_python_path",
                    {"Interpreter with fastembed installed; empty = python3 on PATH."});
    reader.Describe("sidecar_model", {"fastembed model, e.g. BAAI/bge-small-en-v1.5. Changing "
                                      "it invalidates stored vectors."});
    reader.Describe("sidecar_startup_timeout_ms", {"Sidecar spawn + model load window."});
    reader.Describe("sidecar_request_timeout_ms", {"One sidecar embedding request's bound."});
    reader.Describe("tagger_model", {"<provider>/<model> that files entries under tags; empty = "
                                     "entries stay untagged."});
    reader.Describe("tagger_max_tokens", {"Output cap for one tagging request.", kLive});
    reader.Describe("tagger_timeout_ms", {"One tagging request's bound.", kLive});
    reader.Describe("max_tags", {"Tag vocabulary cap per scope.", kLive});
    reader.Describe("max_tags_per_entry", {"Tags one entry may carry.", kLive});
    reader.Describe("tag_merge_similarity",
                    {"Embedding similarity at which a new tag merges into an existing one.", kLive});
    reader.Describe("allow_inline", {"false = file-only: entries enter only through "
                                     "memory_store(path=...)."});
    reader.Describe("max_text_bytes", {"Largest entry text.", kLive});
    reader.Describe("batch_size", {"Entries embedded per backend call."});
    reader.Describe("default_limit", {"Search results when the model supplies no limit.", kLive});
    reader.Describe("writes_prompt", {"false = memory writes go through without a prompt."});
    reader.Describe("index_enabled", {"Inject the memory index into each session's context.", kLive});
    reader.Describe("index_budget_bytes", {"Size budget of that index.", kLive});
    reader.Describe("index_in_subagents", {"Inject the index into subagents too.", kLive});
    // After the Describes: the schema lists fields in first-touch order.
    for (const char* key : {"api_base", "api_key", "model", "timeout_ms"}) {
        reader.RelevantWhen(key, "strategy", {"http", "auto"});
    }
    for (const char* key : {"sidecar_python_path", "sidecar_model", "sidecar_startup_timeout_ms",
                            "sidecar_request_timeout_ms"}) {
        reader.RelevantWhen(key, "strategy", {"sidecar", "auto"});
    }
    reader.RelevantWhen("batch_size", "strategy", {"http", "sidecar", "auto"});
    for (const char* key : {"tagger_max_tokens", "tagger_timeout_ms", "max_tags",
                            "max_tags_per_entry", "tag_merge_similarity"}) {
        reader.RelevantWhenSet(key, "tagger_model");
    }

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
