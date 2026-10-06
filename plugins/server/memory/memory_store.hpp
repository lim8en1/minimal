#pragma once

// MemoryStore -- the plugin's own SQLite database (schema v2). PluginStorage
// is the KV escape hatch; this needs FTS5 ranking, structured provenance and
// migrations, so it owns its own connection under data_dir.
//
// One connection, guarded by mu_: tool threads, the embedding worker and the
// tagging worker all come through here. The store also owns the in-memory
// vector cache, because it is the single writer and so the one place that can
// keep the cache coherent with the rows.

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "embedding.hpp"

struct sqlite3;

namespace minimal::store {
class ExclusiveFileLock;
}

namespace minimal::memory {

// The scope column's stored values (schema facts).
inline constexpr int kScopeGlobal = 0;   // NOLINT(readability-magic-numbers): stored value
inline constexpr int kScopeProject = 1;  // NOLINT(readability-magic-numbers): stored value

// Which rows a caller may see. Global rows are visible from everywhere;
// project rows only from their own project root.
struct ScopeFilter {
    bool include_global = true;
    bool include_project = true;
    std::string project_root;
};

struct NewEntry {
    int scope = kScopeProject;
    std::string project_root;  // worktree root when scope == project, "" for global
    std::string session_id;    // provenance, may be empty
    std::string origin = "model";  // "model" | "ingest"
    std::string origin_ref;    // source path for ingest, may be empty
    std::string title;
    std::string description;
    std::string body;
    bool pinned = false;
};

struct EntryRow {
    std::string id;
    int scope = kScopeProject;
    std::string project_root;
    std::string session_id;
    std::string origin;
    std::string origin_ref;
    std::optional<std::string> group_id;
    int chunk_index = 0;
    int chunk_count = 0;
    std::string title;
    std::string description;
    std::string body;
    bool pinned = false;
    std::vector<std::string> tags;
    std::int64_t created_ms = 0;
    std::int64_t updated_ms = 0;
};

struct EntryPatch {
    std::optional<std::string> title, description, body;
    std::optional<bool> pinned;
};

enum class WriteOutcome { kDone, kNotFound, kIngestChunk };

struct LexicalHit {
    std::string id;
    double rank = 0;      // bm25 (lower is better) or substring term count (higher)
    std::string snippet;  // body excerpt with [matches]; empty for substring hits
};

struct VectorHit {
    std::string id;
    double score = 0;  // cosine
};

struct TagInfo {
    std::int64_t id = 0;
    int scope = kScopeProject;
    std::string project_root;
    std::string name;
    std::string description;
    int count = 0;
};

struct PendingEmbedRow {
    std::string id;
    std::string text;  // title + description + body, the embedded form
};

struct PendingTagRow {
    std::string id;
    int scope = kScopeProject;
    std::string project_root;
    std::optional<std::string> group_id;
    std::string title, description, body;
};

struct StoreStats {
    std::map<std::string, int> entries_by_scope;  // "project" / "global"
    std::map<std::string, int> by_origin;
    int pinned = 0;
    int pending_embed = 0;
    int failed_embed = 0;
    int pending_tagging = 0;
    int tags = 0;
};

// Tag names: lowercase kebab, <= 32 bytes, UTF-8 letters kept. The match key
// additionally folds a plural "s", so "Unit-Tests", "unit_test" and
// "unit tests" are one tag.
std::string NormalizeTagName(std::string_view raw);
std::string TagMatchKey(std::string_view normalized);

// The text an embedding is computed over. One definition, used by the store
// (pending rows) and the tools (eager embedding on store).
std::string EmbedText(const std::string& title, const std::string& description,
                      const std::string& body);

class MemoryStore {
public:
    // Opens (creating if needed). Throws std::runtime_error on any failure,
    // including a store at any other schema version: never rebuild over it.
    explicit MemoryStore(const std::filesystem::path& db_path);
    ~MemoryStore();

    MemoryStore(const MemoryStore&) = delete;
    MemoryStore& operator=(const MemoryStore&) = delete;

    static constexpr int kSchemaVersion = 2;  // NOLINT(readability-magic-numbers): the schema's own version

    // ---------------------------------------------------------------- writes
    std::string Insert(const NewEntry& entry);

    // Replaces every entry previously ingested from `origin_ref` in this scope
    // (the refinement loop: re-ingesting a file updates, never accumulates).
    struct IngestResult {
        std::string group_id;
        std::vector<std::string> ids;
        int replaced = 0;
    };
    IngestResult Ingest(const std::vector<NewEntry>& chunks, const std::string& origin_ref);

    // In place: id, created_at and provenance survive. Text edits re-queue
    // embedding and tagging. Ingest chunks are refused (edit the file).
    WriteOutcome Update(const std::string& id, const EntryPatch& patch, const ScopeFilter& filter);
    // Ingest chunks are refused: deleting one would be undone by the next
    // re-ingest of its file. DeleteGroup removes a whole file's entries.
    WriteOutcome Delete(const std::string& id, const ScopeFilter& filter);
    // By group id or by origin_ref (the source path). Returns entries removed.
    int DeleteGroup(const std::string& group_or_ref, const ScopeFilter& filter);

    // ----------------------------------------------------------------- reads
    std::optional<EntryRow> Get(const std::string& id, const ScopeFilter& filter);
    // Order-preserving; ids not visible under `filter` are skipped.
    std::vector<EntryRow> GetMany(const std::vector<std::string>& ids, const ScopeFilter& filter);

    struct Filters {
        ScopeFilter scope;
        std::vector<std::string> tags;  // AND; matched by tag match key
    };
    // Word-level FTS5 with bm25 over title/description/tags/body (OR of the
    // query's words, prefix-matched). When that finds nothing -- a
    // language without word boundaries, a substring of a longer word -- a
    // substring scan ranks rows by how many query terms they contain.
    std::vector<LexicalHit> Lexical(const std::string& query, const Filters& filters, int limit);
    // Filter-only listing, pinned first, then most recently updated.
    std::vector<std::string> Browse(const Filters& filters, int limit);
    // Ids passing `filters`, for intersecting vector candidates. nullopt when
    // there is no tag filter (the vector scan checks scope itself).
    std::optional<std::unordered_set<std::string>> AllowedIds(const Filters& filters);

    // Cosine nearest neighbors over cached unit vectors of `signature`.
    std::vector<VectorHit> Nearest(const std::vector<float>& query, const std::string& signature,
                                   const ScopeFilter& filter, int k,
                                   const std::unordered_set<std::string>* allowed = nullptr);

    // ------------------------------------------------------ embedding pipeline
    std::vector<PendingEmbedRow> PendingEmbeddings(int limit);
    void StoreEmbedding(const std::string& id, const Embedding& embedding,
                        const std::string& signature);
    void MarkEmbedFailed(const std::vector<std::string>& ids, const std::string& error);
    int ResetFailedEmbeddings();
    // The ONE re-embed trigger: rows embedded under another signature go back
    // to pending. Run once when the runtime starts.
    int ResetStaleSignatures(const std::string& signature);

    // ------------------------------------------------------------------ tags
    std::vector<TagInfo> Tags(int scope, const std::string& project_root);
    std::optional<TagInfo> FindTag(int scope, const std::string& project_root,
                                   const std::string& name);
    int TagCount(int scope, const std::string& project_root);
    // Creates, or returns the existing tag with the same match key.
    std::int64_t CreateTag(int scope, const std::string& project_root, const std::string& name,
                           const std::string& description);
    // Replaces the entry's tags and marks it tagged.
    void SetEntryTags(const std::string& entry_id, const std::vector<std::int64_t>& tag_ids);
    // 0 = pending, 1 = tagged, 2 = skipped (the tagger gave up on it).
    void SetTagStatus(const std::vector<std::string>& ids, int status);
    std::vector<PendingTagRow> PendingTagging(int limit);
    // Operator maintenance. Merge moves every entry of `from` onto `to` and
    // drops `from`; both must exist in the scope.
    bool MergeTags(int scope, const std::string& project_root, const std::string& from,
                   const std::string& to);
    bool RenameTag(int scope, const std::string& project_root, const std::string& from,
                   const std::string& to);

    // ---------------------------------------------------- index + operator
    std::vector<EntryRow> Pinned(const ScopeFilter& filter);
    std::vector<EntryRow> Recent(const ScopeFilter& filter, int limit);
    StoreStats Stats(const ScopeFilter& filter);

private:
    void Migrate();
    std::vector<EntryRow> HydrateLocked(const std::vector<std::string>& ids,
                                        const ScopeFilter& filter);
    void DropUnusedTagsLocked();
    void EnsureVectorCacheLocked(const std::string& signature);
    void RefreshTagsTextLocked(std::int64_t rowid);

    struct CachedVector {
        int scope = kScopeProject;
        std::string project_root;
        std::vector<float> unit;
    };

    std::unique_ptr<store::ExclusiveFileLock> lock_;  // declared before db_: outlives it
    sqlite3* db_ = nullptr;
    mutable std::mutex mu_;
    std::filesystem::path path_;

    std::string cache_signature_;
    bool cache_loaded_ = false;
    std::unordered_map<std::string, CachedVector> cache_;
};

}  // namespace minimal::memory
