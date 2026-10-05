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
    std::string api_key;         // key reference ("env:X", "secret:X"); empty = no auth header
    std::string model;           // backend-side model name, e.g. "nomic-embed-text"
    int timeout_ms = 30000;      // whole-transfer cap per embeddings call

    // --- sidecar backend (fastembed via a long-lived python subprocess) ---
    std::string sidecar_python_path;  // absolute; empty = resolve "python3" on PATH
    std::string sidecar_model;        // fastembed model name, e.g. "BAAI/bge-small-en-v1.5"; no default
    int sidecar_startup_timeout_ms = 60000;  // first run may download model weights
    int sidecar_request_timeout_ms = 30000;  // per Embed() round trip

    // --- tagging (a cheap model files entries into the tag vocabulary) ---
    // "<provider>/<model>" resolved through SessionSupportService. EMPTY =
    // tagging off: entries stay untagged and remain fully searchable.
    std::string tagger_model;
    int tagger_max_tokens = 512;             // per tagging call (a batch of entries)
    int tagger_timeout_ms = 60000;           // bounds one provider call
    int max_tags = 100;                      // vocabulary cap per scope
    int max_tags_per_entry = 5;
    double tag_merge_similarity = 0.85;      // embed-dedup threshold for a proposed new tag

    // --- store limits ---
    bool allow_inline = true;    // false = file-ingestion-only, DB-backed mode
    int max_text_bytes = 8192;   // per entry body (and per ingest chunk)
    int batch_size = 64;         // inputs per embeddings request
    int default_limit = 12;      // search result count; ceiling 64
    // Write tools (store/update/delete) are FileWrite by default so they
    // follow the operator's write-permission policy -- a stored memory
    // outlives the session and, at global scope, follows the user into every
    // project. false restores the old prompt-free (ReadOnly) behavior.
    bool writes_prompt = true;

    // --- context index (the "Memory" section pre-filled into sessions) ---
    bool index_enabled = true;
    // ONE budget for the whole section (~4 bytes/token for English), filled
    // in priority order: pinned bodies (up to half), the tag vocabulary, then
    // recent one-liners. Whatever does not fit is announced with a count and
    // how to fetch it, never silently dropped.
    int index_budget_bytes = 4000;
    bool index_in_subagents = false;
};

MemorySettings ParseMemorySettings(const nlohmann::json& config);

}  // namespace minimal::memory
