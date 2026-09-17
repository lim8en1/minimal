#pragma once

// MemoryStore -- the plugin's own SQLite database. PluginStorage is the KV
// escape hatch; this needs FTS5 ranking, structured provenance columns and
// migrations, so it owns its own connection under data_dir (the per-plugin
// DB precedent: "a plugin needing SQL opens its own database there").

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "embedding.hpp"
#include "memory_settings.hpp"

struct sqlite3;

namespace minimal::common {
class ExclusiveFileLock;
}

namespace minimal::memory {

struct SearchHit {
    std::string id;
    std::string preview;      // first 128 code points of text
    std::optional<std::string> summary;
    std::optional<std::string> topic;
    std::string created_at;   // ISO-8601 UTC
    std::int64_t created_ms = 0;
    std::string project_root; // scope=project only
    double score = 0.0;       // similarity 0..1 (sim) or recency rank 1..n (time)
};

struct Digest {
    std::map<std::string, int> entries_by_scope;   // "project"/"global"
    std::map<std::string, int> by_origin;          // model/ingest/compaction
    std::vector<std::pair<std::string, int>> top_topics;  // count > 0, desc
    std::string backend;                           // Describe() or "lexical only"
    std::string enrichment;                        // "on (<model>)" / "off"
    int pending_reindex = 0;
};

// One entry to store: a lesson. The store decides nothing about scope
// resolution -- the caller (tool) resolves project root and passes it in.
struct NewEntry {
    int scope = 0;               // 0 global, 1 project
    std::string project_root;    // worktree root when scope==1
    std::string session_id;      // provenance, may be empty
    std::string origin;          // "model" | "ingest" | "compaction"
    std::string origin_ref;      // provenance pointer (path / session id), may be empty
    std::string text;
    std::optional<std::string> group_id;  // set for ingest chunks
};

// Cosine similarity over two equal-length vectors; 0.0 when empty/mismatched.
double Cosine(const std::vector<float>& a, const std::vector<float>& b);

class MemoryStore {
public:
    // Opens (creating if needed) and migrates. Throws std::runtime_error on
    // any failure, including a newer-user_version database (the
    // PluginStorage/SessionDatabase discipline: never rebuild over a newer
    // store). One connection, guarded by mu_: the tools and the enrichment
    // worker hit it from different threads.
    explicit MemoryStore(const std::filesystem::path& db_path);
    ~MemoryStore();

    MemoryStore(const MemoryStore&) = delete;
    MemoryStore& operator=(const MemoryStore&) = delete;

    // --- writes (tool thread) ---
    // Returns the new entry id (ULID).
    std::string Insert(const NewEntry& entry);

    // Ingest: replaces the entries of `origin_ref` (same path) in one
    // transaction, creating the group row; returns {group_id, ids}. The
    // replace is the refinement loop -- a re-ingested file updates rather
    // than accumulates.
    struct IngestResult {
        std::string group_id;
        std::vector<std::string> ids;
        int replaced = 0;
    };
    IngestResult Ingest(const NewEntry& first, const std::vector<NewEntry>& chunks,
                        const std::string& origin_ref);

    bool Delete(const std::string& id);

    // /memory purge: remove every entry of one ingest group (by origin_ref),
    // across scopes of the caller's choosing -- scoped to the given pair.
    int PurgeByOriginRef(const std::string& origin_ref, int scope,
                         const std::string& project_root);

    // Enrichment write-back: sets summary and/or topic on existing rows.
    void SetSummary(const std::string& id, const std::string& summary);
    // Topic by name; binds the entry to it. Match is case-insensitive;
    // create_if_missing creates the topic when under cap (returns false at
    // cap, entry stays unfiled -- the taxonomy's one hard rule).
    bool BindTopic(const std::string& entry_id, int scope, const std::string& project_root,
                   const std::string& name, bool create_if_missing);

    // --- embedding pipeline ---
    // Rows with embed_status=0 (pending) after a strategy-signature change,
    // up to a budget; id + text. Marks them in-flight (3).
    struct PendingRow { std::string id; std::string text; };
    std::vector<PendingRow> ClaimPendingEmbeddings(const std::string& signature, int budget);
    // Marks failed rows back to pending and stores the reason.
    void MarkEmbedFailed(const std::vector<std::string>& ids, const std::string& error);
    void StoreEmbedding(const std::string& id, const Embedding& embedding);
    // Returns nearest by cosine over rows matching signature; empty when no
    // embeddings exist yet. Used by search (sim sort) and the similar-hint.
    struct VectorRow { std::string id; std::vector<float> values; };
    std::vector<VectorRow> LoadVectors(const std::string& signature, int scope,
                                       const std::string& project_root);
    int SetSignatureForAll(const std::string& signature, int dim);

    // --- reads (tool thread) ---
    struct SearchQuery {
        int scope = 0;
        std::string project_root;
        std::optional<std::string> query;       // FTS match text
        std::vector<std::string> keywords;      // AND, substring vs text/origin_ref
        std::optional<std::string> topic;
        std::string sort;                        // "sim" | "time"
        int limit = 12;
        std::string signature;                   // current strategy signature
        bool has_embeddings = false;             // whether sim sort is possible
    };
    std::vector<SearchHit> Search(const SearchQuery& query);

    struct Entry {
        std::string id, text, scope_name, project_root, session_id, origin, origin_ref;
        std::optional<std::string> summary, topic, origin_ref_display;
        std::int64_t created_ms = 0;
        std::string created_at;
        std::optional<std::string> group_id;
        int chunk_index = 0, chunk_count = 0;
        std::vector<std::string> sibling_ids;
    };
    std::optional<Entry> Get(const std::string& id);

    Digest DigestInfo(const std::string& backend, const std::string& enrichment);

    // Topics for the enrichment prompt: names by recency of use, capped.
    std::vector<std::string> TopicNames(int scope, const std::string& project_root, int cap);
    std::vector<std::pair<std::string, int>> AllTopics(int scope, const std::string& project_root);

    static constexpr int kScopeGlobal = 0;
    static constexpr int kScopeProject = 1;

private:
    void Migrate();
    std::string Iso8601(std::int64_t ms) const;
    void SyncFts(std::int64_t rowid, const std::string& text);

    sqlite3* db_ = nullptr;
    std::unique_ptr<common::ExclusiveFileLock> lock_;  // declared before db_: outlives it
    mutable std::mutex mu_;
    std::filesystem::path path_;
};

}  // namespace minimal::memory
