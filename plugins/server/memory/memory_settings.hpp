#pragma once

// ParseMemorySettings -- the tools-web shape: lenient where a value is
// cosmetic (a bad optional number falls back to the default), strict where
// structural (missing strategy inputs leave that backend unconfigured, which
// the strategy resolver reports, not a load failure).

#include <nlohmann/json.hpp>
#include <string>

namespace minimal::memory {

// Which embedding backend the store uses. "auto" resolves at registration:
// sidecar when sidecar_model is set, else http when api_base is set, else
// none.
enum class StrategyKind { kAuto, kHttp, kSidecar, kNone };

struct MemorySettings {
    StrategyKind strategy = StrategyKind::kAuto;

    // --- http backend ---
    std::string api_base;        // e.g. "http://127.0.0.1:8080/v1"; no default
    std::string api_key_env;     // env var NAME; empty = no auth header
    std::string model;           // backend-side model name, e.g. "nomic-embed-text"
    int timeout_ms = 30000;      // whole-transfer cap per embeddings call

    // --- sidecar backend (fastembed via a long-lived python subprocess) ---
    std::string sidecar_python_path;  // absolute; empty = resolve "python3" on PATH
    std::string sidecar_model;        // fastembed model name, e.g. "BAAI/bge-small-en-v1.5"; no default
    int sidecar_startup_timeout_ms = 60000;  // first run may download model weights
    int sidecar_request_timeout_ms = 30000;  // per Embed() round trip

    // --- enrichment (small LLM summary + topic attribution) ---
    // "<provider>/<model>" resolved through SessionSupportService. EMPTY =
    // enrichment off (the session-titler inversion: titler's empty means
    // "session's own model" because titling is its whole job; memory's
    // enrichment is optional garnish on a fully-functional store, so empty
    // means off and naming a model opts in).
    std::string summarizer_model;
    int summarize_max_tokens = 64;
    int summarize_input_char_budget = 2000;  // code-point budget, titler rule
    int max_topics = 4096;                   // per scope; creation refused at cap

    // --- store limits ---
    bool allow_inline = true;    // false = file-ingestion-only, DB-backed mode
    int max_text_bytes = 8192;
    int batch_size = 64;         // inputs per embeddings request
    int reindex_budget_rows = 50;   // lazy re-embed cap per idle drain
    int default_limit = 12;      // search result count; ceiling 64

    // Digest gate: first memory call of a session prepends the digest.
    bool digest_enabled = true;
};

MemorySettings ParseMemorySettings(const nlohmann::json& config);

}  // namespace minimal::memory
