#include "memory_store.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

#include <sqlite3.h>

#include "minimal/store/file_lock.hpp"
#include "minimal/common/clock.hpp"
#include "minimal/store/sqlite_util.hpp"
#include "minimal/common/utf8.hpp"
#include "minimal/session/ulid.hpp"

namespace minimal::memory {
namespace {

using store::ExecChecked;
using store::Stmt;
using store::ThrowSqliteError;

// Tables that did not change shape between v1 and v2.
constexpr const char* kCreateAuxSql = R"sql(
CREATE TABLE memory_ingest_groups (
    id           TEXT PRIMARY KEY,
    scope        INTEGER NOT NULL,
    project_root TEXT NOT NULL DEFAULT '',
    origin_ref   TEXT NOT NULL,
    chunk_count  INTEGER NOT NULL,
    created_ms   INTEGER NOT NULL
);
CREATE TABLE memory_meta (key TEXT PRIMARY KEY, value TEXT NOT NULL);
)sql";

// tags_text is a denormalized copy of the entry's tag names: FTS5 external
// content reads indexed columns from the content table by name, so the tags
// have to exist there as a column to be searchable. SetEntryTags keeps it in
// sync. The triggers are the standard external-content recipe; every text
// write goes through the store, never raw SQL.
//
// unicode61 (word tokens, diacritics folded) replaces v1's trigram tokenizer:
// trigram cannot match a term shorter than three characters, which made every
// natural-language query with a short word ("do", "CI") match nothing.
constexpr const char* kCreateV2Sql = R"sql(
CREATE TABLE memory_entries (
    rowid_       INTEGER PRIMARY KEY,
    id           TEXT UNIQUE NOT NULL,
    scope        INTEGER NOT NULL,
    project_root TEXT NOT NULL DEFAULT '',
    session_id   TEXT NOT NULL DEFAULT '',
    origin       TEXT NOT NULL DEFAULT 'model',
    origin_ref   TEXT,
    ingest_group TEXT,
    chunk_index  INTEGER NOT NULL DEFAULT 0,
    created_ms   INTEGER NOT NULL,
    updated_ms   INTEGER NOT NULL,
    title        TEXT NOT NULL,
    description  TEXT NOT NULL,
    body         TEXT NOT NULL,
    tags_text    TEXT NOT NULL DEFAULT '',
    pinned       INTEGER NOT NULL DEFAULT 0,
    tag_status   INTEGER NOT NULL DEFAULT 0,
    embedding    BLOB,
    embed_status INTEGER NOT NULL DEFAULT 0,
    signature    TEXT,
    dim          INTEGER,
    embed_error  TEXT
);
CREATE INDEX memory_entries_scope ON memory_entries(scope, project_root);
CREATE INDEX memory_entries_embed_pending ON memory_entries(embed_status) WHERE embed_status = 0;
CREATE INDEX memory_entries_tag_pending ON memory_entries(tag_status) WHERE tag_status = 0;
CREATE INDEX memory_entries_by_group ON memory_entries(ingest_group);
CREATE INDEX memory_entries_by_origin_ref ON memory_entries(origin_ref);

CREATE TABLE memory_tags (
    id           INTEGER PRIMARY KEY,
    scope        INTEGER NOT NULL,
    project_root TEXT NOT NULL DEFAULT '',
    name         TEXT NOT NULL,
    match_key    TEXT NOT NULL,
    description  TEXT NOT NULL DEFAULT '',
    created_ms   INTEGER NOT NULL,
    UNIQUE(scope, project_root, match_key)
);
CREATE TABLE memory_entry_tags (
    entry_rowid INTEGER NOT NULL REFERENCES memory_entries(rowid_) ON DELETE CASCADE,
    tag_id      INTEGER NOT NULL REFERENCES memory_tags(id) ON DELETE CASCADE,
    PRIMARY KEY (entry_rowid, tag_id)
) WITHOUT ROWID;
CREATE INDEX memory_entry_tags_by_tag ON memory_entry_tags(tag_id);

CREATE VIRTUAL TABLE memory_fts USING fts5(
    title, description, tags_text, body,
    content='memory_entries', content_rowid='rowid_',
    tokenize='unicode61 remove_diacritics 2'
);
CREATE TRIGGER memory_fts_ai AFTER INSERT ON memory_entries BEGIN
    INSERT INTO memory_fts(rowid, title, description, tags_text, body)
        VALUES (new.rowid_, new.title, new.description, new.tags_text, new.body);
END;
CREATE TRIGGER memory_fts_ad AFTER DELETE ON memory_entries BEGIN
    INSERT INTO memory_fts(memory_fts, rowid, title, description, tags_text, body)
        VALUES ('delete', old.rowid_, old.title, old.description, old.tags_text, old.body);
END;
CREATE TRIGGER memory_fts_au AFTER UPDATE OF title, description, tags_text, body ON memory_entries BEGIN
    INSERT INTO memory_fts(memory_fts, rowid, title, description, tags_text, body)
        VALUES ('delete', old.rowid_, old.title, old.description, old.tags_text, old.body);
    INSERT INTO memory_fts(rowid, title, description, tags_text, body)
        VALUES (new.rowid_, new.title, new.description, new.tags_text, new.body);
END;
)sql";

// Embedding models have a context limit (512 tokens is common for small
// local ones; an http server may reject longer input outright), so the
// embedded form is capped. Title and description lead, so the cap only ever
// trims the tail of the body -- the part that matters least for "what is
// this entry about".
constexpr std::size_t kEmbedTextMaxChars = 2000;

constexpr const char* kEntryColumns =
    "e.rowid_, e.id, e.scope, e.project_root, e.session_id, e.origin, e.origin_ref, "
    "e.ingest_group, e.chunk_index, e.title, e.description, e.body, e.pinned, e.created_ms, "
    "e.updated_ms";

std::string ColText(sqlite3_stmt* stmt, int col) {
    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
    return text != nullptr ? std::string(text, static_cast<std::size_t>(sqlite3_column_bytes(stmt, col)))
                           : std::string();
}

void BindText(sqlite3_stmt* stmt, int index, const std::string& value) {
    sqlite3_bind_text(stmt, index, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
}

void StepDone(sqlite3* db, sqlite3_stmt* stmt, const std::string& context) {
    if (sqlite3_step(stmt) != SQLITE_DONE) ThrowSqliteError(db, context);
}

// Named-parameter collector for the dynamically composed search SQL: each
// Add returns a fresh ":pN" name to splice into the text, and Bind resolves
// them all after prepare.
class Params {
public:
    std::string Add(std::string value) {
        std::string name = ":p" + std::to_string(values_.size());
        values_.emplace_back(name, std::move(value));
        return name;
    }
    std::string Add(std::int64_t value) {
        std::string name = ":p" + std::to_string(values_.size());
        values_.emplace_back(name, value);
        return name;
    }
    void Bind(sqlite3_stmt* stmt) const {
        for (const auto& [name, value] : values_) {
            const int index = sqlite3_bind_parameter_index(stmt, name.c_str());
            if (index == 0) continue;
            if (const auto* text = std::get_if<std::string>(&value)) {
                BindText(stmt, index, *text);
            } else {
                sqlite3_bind_int64(stmt, index, std::get<std::int64_t>(value));
            }
        }
    }

private:
    std::vector<std::pair<std::string, std::variant<std::int64_t, std::string>>> values_;
};

std::string AsciiLower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string ScopeSql(const ScopeFilter& filter, Params& params) {
    if (filter.include_global && filter.include_project) {
        return "(e.scope = 0 OR (e.scope = 1 AND e.project_root = " +
               params.Add(filter.project_root) + "))";
    }
    if (filter.include_global) return "e.scope = 0";
    if (filter.include_project) {
        return "(e.scope = 1 AND e.project_root = " + params.Add(filter.project_root) + ")";
    }
    return "0";
}

std::string FiltersSql(const MemoryStore::Filters& filters, Params& params) {
    std::string sql = ScopeSql(filters.scope, params);
    for (const auto& tag : filters.tags) {
        const std::string key = TagMatchKey(NormalizeTagName(tag));
        if (key.empty()) continue;
        sql += " AND EXISTS (SELECT 1 FROM memory_entry_tags et JOIN memory_tags t "
               "ON t.id = et.tag_id WHERE et.entry_rowid = e.rowid_ AND t.match_key = " +
               params.Add(key) + ")";
    }
    return sql;
}

bool Visible(const ScopeFilter& filter, int scope, const std::string& project_root) {
    if (scope == kScopeGlobal) return filter.include_global;
    return filter.include_project && project_root == filter.project_root;
}

// Query words for FTS5: runs of ASCII alphanumerics and non-ASCII bytes
// (UTF-8 letters), lowercased, deduplicated. Each becomes a quoted, prefix-
// matched term, OR-ed: bm25 does the ranking, so a query need not have every
// word present -- v1 ANDed every word and so found nothing for most
// natural-language questions.
std::string FtsOrQuery(std::string_view query) {
    std::vector<std::string> terms;
    std::string current;
    auto flush = [&] {
        // Single ASCII letters ("a", "I") are pure noise under OR.
        if (current.size() >= 2 &&
            std::find(terms.begin(), terms.end(), current) == terms.end()) {
            terms.push_back(current);
        }
        current.clear();
    };
    for (const char raw : query) {
        const unsigned char c = static_cast<unsigned char>(raw);
        if (std::isalnum(c) || c >= 0x80) {
            current += static_cast<char>(std::tolower(c));
        } else {
            flush();
        }
    }
    flush();
    if (terms.size() > 32) terms.resize(32);

    std::string out;
    for (const auto& term : terms) {
        if (!out.empty()) out += " OR ";
        out += '"' + term + '"';
        if (term.size() >= 3) out += '*';
    }
    return out;
}

std::vector<std::string> SubstringTerms(std::string_view query) {
    std::vector<std::string> terms;
    std::size_t i = 0;
    while (i < query.size()) {
        while (i < query.size() && std::isspace(static_cast<unsigned char>(query[i]))) ++i;
        const std::size_t start = i;
        while (i < query.size() && !std::isspace(static_cast<unsigned char>(query[i]))) ++i;
        if (i > start) terms.push_back(AsciiLower(query.substr(start, i - start)));
    }
    if (terms.size() > 16) terms.resize(16);
    return terms;
}

std::vector<float> DecodeBlob(const void* data, int bytes) {
    std::vector<float> out;
    if (data != nullptr && bytes > 0) {
        out.resize(static_cast<std::size_t>(bytes) / sizeof(float));
        std::memcpy(out.data(), data, out.size() * sizeof(float));
    }
    return out;
}

bool Normalize(std::vector<float>& values) {
    double norm = 0;
    for (const float v : values) norm += static_cast<double>(v) * v;
    if (norm <= 0) return false;
    const double inv = 1.0 / std::sqrt(norm);
    for (float& v : values) v = static_cast<float>(v * inv);
    return true;
}

std::int64_t FindTagIdLocked(sqlite3* db, int scope, const std::string& project_root,
                             const std::string& key) {
    Stmt stmt(db, "SELECT id FROM memory_tags WHERE scope = ?1 AND project_root = ?2 "
                  "AND match_key = ?3;",
              "memory: find tag");
    sqlite3_bind_int(stmt.stmt, 1, scope);
    BindText(stmt.stmt, 2, project_root);
    BindText(stmt.stmt, 3, key);
    return sqlite3_step(stmt.stmt) == SQLITE_ROW ? sqlite3_column_int64(stmt.stmt, 0) : 0;
}

std::int64_t CreateTagLocked(sqlite3* db, int scope, const std::string& project_root,
                             const std::string& name, const std::string& description) {
    const std::string normalized = NormalizeTagName(name);
    const std::string key = TagMatchKey(normalized);
    if (key.empty()) throw std::runtime_error("memory: empty tag name");
    if (const auto existing = FindTagIdLocked(db, scope, project_root, key)) return existing;
    Stmt stmt(db, "INSERT INTO memory_tags (scope, project_root, name, match_key, description, "
                  "created_ms) VALUES (?1, ?2, ?3, ?4, ?5, ?6);",
              "memory: create tag");
    sqlite3_bind_int(stmt.stmt, 1, scope);
    BindText(stmt.stmt, 2, project_root);
    BindText(stmt.stmt, 3, normalized);
    BindText(stmt.stmt, 4, key);
    BindText(stmt.stmt, 5, description);
    sqlite3_bind_int64(stmt.stmt, 6, common::NowMillis());
    StepDone(db, stmt.stmt, "memory: create tag");
    return sqlite3_last_insert_rowid(db);
}

std::int64_t InsertEntryLocked(sqlite3* db, const std::string& id, const NewEntry& entry,
                               const std::optional<std::string>& group_id, int chunk_index,
                               std::int64_t created_ms, std::int64_t updated_ms) {
    Stmt stmt(db,
              "INSERT INTO memory_entries (id, scope, project_root, session_id, origin, "
              "origin_ref, ingest_group, chunk_index, created_ms, updated_ms, title, "
              "description, body, pinned) VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14);",
              "memory: insert entry");
    BindText(stmt.stmt, 1, id);
    sqlite3_bind_int(stmt.stmt, 2, entry.scope);
    BindText(stmt.stmt, 3, entry.project_root);
    BindText(stmt.stmt, 4, entry.session_id);
    BindText(stmt.stmt, 5, entry.origin);
    if (entry.origin_ref.empty()) sqlite3_bind_null(stmt.stmt, 6);
    else BindText(stmt.stmt, 6, entry.origin_ref);
    if (group_id) BindText(stmt.stmt, 7, *group_id);
    else sqlite3_bind_null(stmt.stmt, 7);
    sqlite3_bind_int(stmt.stmt, 8, chunk_index);
    sqlite3_bind_int64(stmt.stmt, 9, created_ms);
    sqlite3_bind_int64(stmt.stmt, 10, updated_ms);
    BindText(stmt.stmt, 11, entry.title);
    BindText(stmt.stmt, 12, entry.description);
    BindText(stmt.stmt, 13, entry.body);
    sqlite3_bind_int(stmt.stmt, 14, entry.pinned ? 1 : 0);
    StepDone(db, stmt.stmt, "memory: insert entry");
    return sqlite3_last_insert_rowid(db);
}

// Runs `fn` inside BEGIN IMMEDIATE .. COMMIT, rolling back on any throw.
template <typename Fn>
auto InTransaction(sqlite3* db, const char* what, Fn&& fn) -> decltype(fn()) {
    ExecChecked(db, "BEGIN IMMEDIATE;", std::string("memory: begin ") + what);
    try {
        if constexpr (std::is_void_v<decltype(fn())>) {
            fn();
            ExecChecked(db, "COMMIT;", std::string("memory: commit ") + what);
        } else {
            auto result = fn();
            ExecChecked(db, "COMMIT;", std::string("memory: commit ") + what);
            return result;
        }
    } catch (...) {
        store::BestEffortRollback(db);
        throw;
    }
}

}  // namespace

// ------------------------------------------------------------------ helpers

std::string NormalizeTagName(std::string_view raw) {
    std::string out;
    for (const char ch : raw) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (std::isalnum(c)) {
            out += static_cast<char>(std::tolower(c));
        } else if (c >= 0x80) {
            out += ch;  // UTF-8 letters pass through; ASCII case-folding only
        } else if (c == ' ' || c == '_' || c == '-' || c == '.' || c == '/' || c == ':') {
            if (!out.empty() && out.back() != '-') out += '-';
        }
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    out = common::TruncateUtf8Bytes(out, 32);
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out;
}

std::string TagMatchKey(std::string_view normalized) {
    std::string key;
    for (const char c : normalized) {
        if (c != '-') key += c;
    }
    // Plural fold, deliberately crude: it only has to make "tests"/"test" and
    // "notes"/"note" collide, and must not mangle "status" or "analysis".
    const auto ends_with = [&](std::string_view suffix) {
        return key.size() >= suffix.size() &&
               key.compare(key.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    if (key.size() > 3 && ends_with("s") && !ends_with("ss") && !ends_with("us") &&
        !ends_with("is")) {
        key.pop_back();
    }
    return key;
}

std::string EmbedText(const std::string& title, const std::string& description,
                      const std::string& body) {
    return common::TruncateCodePoints(title + "\n" + description + "\n\n" + body,
                                      kEmbedTextMaxChars);
}

// ------------------------------------------------------------ lifecycle

MemoryStore::MemoryStore(const std::filesystem::path& db_path) : path_(db_path) {
    std::filesystem::create_directories(db_path.parent_path());
    // The lock appends ".lock" itself (like PluginStorage/SessionDatabase).
    lock_ = std::make_unique<store::ExclusiveFileLock>(db_path);
    if (sqlite3_open(db_path.string().c_str(), &db_) != SQLITE_OK) {
        const std::string message = db_ != nullptr ? sqlite3_errmsg(db_) : "unknown";
        sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("memory: cannot open store " + db_path.string() + ": " + message);
    }
    try {
        ExecChecked(db_, "PRAGMA journal_mode=WAL;", "memory: journal mode");
        ExecChecked(db_, "PRAGMA foreign_keys=ON;", "memory: foreign keys");
        Migrate();
    } catch (...) {
        sqlite3_close(db_);
        db_ = nullptr;
        throw;
    }
}

MemoryStore::~MemoryStore() {
    if (db_ != nullptr) sqlite3_close(db_);
}

void MemoryStore::Migrate() {
    InTransaction(db_, "migration", [&] {
        int current = 0;
        {
            Stmt stmt(db_, "PRAGMA user_version;", "memory: read schema version");
            if (sqlite3_step(stmt.stmt) == SQLITE_ROW) current = sqlite3_column_int(stmt.stmt, 0);
        }
        if (current > kSchemaVersion) {
            throw std::runtime_error("memory: schema version " + std::to_string(current) +
                                     " is newer than this binary understands; refusing to open");
        }
        if (current != 0 && current != kSchemaVersion) {
            throw std::runtime_error("memory: unsupported schema version " +
                                     std::to_string(current) + "; delete " + path_.string());
        }
        if (current == 0) {
            ExecChecked(db_, kCreateAuxSql, "memory: create aux tables");
            ExecChecked(db_, kCreateV2Sql, "memory: create v2 schema");
        }
        ExecChecked(db_, ("PRAGMA user_version = " + std::to_string(kSchemaVersion) + ";").c_str(),
                    "memory: record schema version");
    });
}

// ------------------------------------------------------------------ writes

std::string MemoryStore::Insert(const NewEntry& entry) {
    const std::string id = minimal::session::GenerateUlid();
    const std::int64_t now = common::NowMillis();
    std::lock_guard<std::mutex> lock(mu_);
    InsertEntryLocked(db_, id, entry, std::nullopt, 0, now, now);
    return id;
}

MemoryStore::IngestResult MemoryStore::Ingest(const std::vector<NewEntry>& chunks,
                                              const std::string& origin_ref) {
    if (chunks.empty()) throw std::runtime_error("memory: ingest needs at least one chunk");
    const NewEntry& first = chunks.front();
    IngestResult result;
    result.group_id = "ig" + minimal::session::GenerateUlid();
    const std::int64_t now = common::NowMillis();

    std::lock_guard<std::mutex> lock(mu_);
    InTransaction(db_, "ingest", [&] {
        {
            Stmt stmt(db_, "SELECT id FROM memory_entries WHERE origin_ref = ?1 AND scope = ?2 "
                           "AND project_root = ?3 AND ingest_group IS NOT NULL;",
                      "memory: find previous ingest rows");
            BindText(stmt.stmt, 1, origin_ref);
            sqlite3_bind_int(stmt.stmt, 2, first.scope);
            BindText(stmt.stmt, 3, first.project_root);
            while (sqlite3_step(stmt.stmt) == SQLITE_ROW) {
                cache_.erase(ColText(stmt.stmt, 0));
                ++result.replaced;
            }
        }
        for (const char* sql : {"DELETE FROM memory_entries WHERE origin_ref = ?1 AND scope = ?2 "
                                "AND project_root = ?3 AND ingest_group IS NOT NULL;",
                                "DELETE FROM memory_ingest_groups WHERE origin_ref = ?1 AND "
                                "scope = ?2 AND project_root = ?3;"}) {
            Stmt stmt(db_, sql, "memory: replace previous ingest");
            BindText(stmt.stmt, 1, origin_ref);
            sqlite3_bind_int(stmt.stmt, 2, first.scope);
            BindText(stmt.stmt, 3, first.project_root);
            StepDone(db_, stmt.stmt, "memory: replace previous ingest");
        }
        {
            Stmt stmt(db_, "INSERT INTO memory_ingest_groups (id, scope, project_root, origin_ref, "
                           "chunk_count, created_ms) VALUES (?1,?2,?3,?4,?5,?6);",
                      "memory: insert ingest group");
            BindText(stmt.stmt, 1, result.group_id);
            sqlite3_bind_int(stmt.stmt, 2, first.scope);
            BindText(stmt.stmt, 3, first.project_root);
            BindText(stmt.stmt, 4, origin_ref);
            sqlite3_bind_int(stmt.stmt, 5, static_cast<int>(chunks.size()));
            sqlite3_bind_int64(stmt.stmt, 6, now);
            StepDone(db_, stmt.stmt, "memory: insert ingest group");
        }
        for (std::size_t i = 0; i < chunks.size(); ++i) {
            NewEntry chunk = chunks[i];
            chunk.origin = "ingest";
            chunk.origin_ref = origin_ref;
            const std::string id = minimal::session::GenerateUlid();
            InsertEntryLocked(db_, id, chunk, result.group_id, static_cast<int>(i), now, now);
            result.ids.push_back(id);
        }
        DropUnusedTagsLocked();
    });
    return result;
}

WriteOutcome MemoryStore::Update(const std::string& id, const EntryPatch& patch,
                                 const ScopeFilter& filter) {
    std::lock_guard<std::mutex> lock(mu_);
    std::int64_t rowid = 0;
    {
        Stmt stmt(db_, "SELECT rowid_, scope, project_root, ingest_group FROM memory_entries "
                       "WHERE id = ?1;",
                  "memory: find entry for update");
        BindText(stmt.stmt, 1, id);
        if (sqlite3_step(stmt.stmt) != SQLITE_ROW ||
            !Visible(filter, sqlite3_column_int(stmt.stmt, 1), ColText(stmt.stmt, 2))) {
            return WriteOutcome::kNotFound;
        }
        if (sqlite3_column_type(stmt.stmt, 3) != SQLITE_NULL) return WriteOutcome::kIngestChunk;
        rowid = sqlite3_column_int64(stmt.stmt, 0);
    }
    const bool text_changed = patch.title || patch.description || patch.body;
    Params params;
    std::string sql = "UPDATE memory_entries SET updated_ms = " + params.Add(common::NowMillis());
    if (patch.title) sql += ", title = " + params.Add(*patch.title);
    if (patch.description) sql += ", description = " + params.Add(*patch.description);
    if (patch.body) sql += ", body = " + params.Add(*patch.body);
    if (patch.pinned) sql += ", pinned = " + params.Add(static_cast<std::int64_t>(*patch.pinned));
    // New text: stale vector, stale tags. Existing tags stay until the tagger
    // re-files it, so the entry is never briefly untagged.
    if (text_changed) sql += ", embed_status = 0, embedding = NULL, tag_status = 0";
    sql += " WHERE rowid_ = " + params.Add(rowid) + ";";
    Stmt stmt(db_, sql.c_str(), "memory: update entry");
    params.Bind(stmt.stmt);
    StepDone(db_, stmt.stmt, "memory: update entry");
    if (text_changed) cache_.erase(id);
    return WriteOutcome::kDone;
}

WriteOutcome MemoryStore::Delete(const std::string& id, const ScopeFilter& filter) {
    std::lock_guard<std::mutex> lock(mu_);
    {
        Stmt stmt(db_, "SELECT scope, project_root, ingest_group FROM memory_entries WHERE id = ?1;",
                  "memory: find entry for delete");
        BindText(stmt.stmt, 1, id);
        if (sqlite3_step(stmt.stmt) != SQLITE_ROW ||
            !Visible(filter, sqlite3_column_int(stmt.stmt, 0), ColText(stmt.stmt, 1))) {
            return WriteOutcome::kNotFound;
        }
        if (sqlite3_column_type(stmt.stmt, 2) != SQLITE_NULL) return WriteOutcome::kIngestChunk;
    }
    InTransaction(db_, "delete", [&] {
        Stmt stmt(db_, "DELETE FROM memory_entries WHERE id = ?1;", "memory: delete entry");
        BindText(stmt.stmt, 1, id);
        StepDone(db_, stmt.stmt, "memory: delete entry");
        DropUnusedTagsLocked();
    });
    cache_.erase(id);
    return WriteOutcome::kDone;
}

int MemoryStore::DeleteGroup(const std::string& group_or_ref, const ScopeFilter& filter) {
    std::lock_guard<std::mutex> lock(mu_);
    // Resolve to the group ids visible under `filter` first: a path may name
    // one group per scope.
    std::vector<std::string> groups;
    {
        Stmt stmt(db_, "SELECT id, scope, project_root FROM memory_ingest_groups "
                       "WHERE id = ?1 OR origin_ref = ?1;",
                  "memory: find ingest groups");
        BindText(stmt.stmt, 1, group_or_ref);
        while (sqlite3_step(stmt.stmt) == SQLITE_ROW) {
            if (Visible(filter, sqlite3_column_int(stmt.stmt, 1), ColText(stmt.stmt, 2))) {
                groups.push_back(ColText(stmt.stmt, 0));
            }
        }
    }
    if (groups.empty()) return 0;
    return InTransaction(db_, "delete group", [&] {
        int removed = 0;
        for (const auto& group : groups) {
            {
                Stmt ids(db_, "SELECT id FROM memory_entries WHERE ingest_group = ?1;",
                         "memory: group ids");
                BindText(ids.stmt, 1, group);
                while (sqlite3_step(ids.stmt) == SQLITE_ROW) cache_.erase(ColText(ids.stmt, 0));
            }
            Stmt stmt(db_, "DELETE FROM memory_entries WHERE ingest_group = ?1;",
                      "memory: delete group entries");
            BindText(stmt.stmt, 1, group);
            StepDone(db_, stmt.stmt, "memory: delete group entries");
            removed += sqlite3_changes(db_);
            Stmt group_row(db_, "DELETE FROM memory_ingest_groups WHERE id = ?1;",
                           "memory: delete group");
            BindText(group_row.stmt, 1, group);
            StepDone(db_, group_row.stmt, "memory: delete group");
        }
        DropUnusedTagsLocked();
        return removed;
    });
}

// ------------------------------------------------------------------- reads

std::vector<EntryRow> MemoryStore::HydrateLocked(const std::vector<std::string>& ids,
                                                 const ScopeFilter& filter) {
    if (ids.empty()) return {};
    Params params;
    std::string in;
    for (const auto& id : ids) {
        if (!in.empty()) in += ", ";
        in += params.Add(id);
    }
    std::unordered_map<std::string, EntryRow> by_id;
    std::unordered_map<std::int64_t, std::string> id_by_rowid;
    {
        const std::string sql = std::string("SELECT ") + kEntryColumns +
                                ", g.chunk_count FROM memory_entries e LEFT JOIN "
                                "memory_ingest_groups g ON g.id = e.ingest_group WHERE e.id IN (" +
                                in + ");";
        Stmt stmt(db_, sql.c_str(), "memory: hydrate entries");
        params.Bind(stmt.stmt);
        while (sqlite3_step(stmt.stmt) == SQLITE_ROW) {
            EntryRow row;
            const std::int64_t rowid = sqlite3_column_int64(stmt.stmt, 0);
            row.id = ColText(stmt.stmt, 1);
            row.scope = sqlite3_column_int(stmt.stmt, 2);
            row.project_root = ColText(stmt.stmt, 3);
            if (!Visible(filter, row.scope, row.project_root)) continue;
            row.session_id = ColText(stmt.stmt, 4);
            row.origin = ColText(stmt.stmt, 5);
            row.origin_ref = ColText(stmt.stmt, 6);
            if (sqlite3_column_type(stmt.stmt, 7) != SQLITE_NULL) row.group_id = ColText(stmt.stmt, 7);
            row.chunk_index = sqlite3_column_int(stmt.stmt, 8);
            row.title = ColText(stmt.stmt, 9);
            row.description = ColText(stmt.stmt, 10);
            row.body = ColText(stmt.stmt, 11);
            row.pinned = sqlite3_column_int(stmt.stmt, 12) != 0;
            row.created_ms = sqlite3_column_int64(stmt.stmt, 13);
            row.updated_ms = sqlite3_column_int64(stmt.stmt, 14);
            row.chunk_count = sqlite3_column_int(stmt.stmt, 15);
            id_by_rowid[rowid] = row.id;
            by_id.emplace(row.id, std::move(row));
        }
    }
    if (!id_by_rowid.empty()) {
        Params tag_params;
        std::string rowids;
        for (const auto& [rowid, id] : id_by_rowid) {
            if (!rowids.empty()) rowids += ", ";
            rowids += tag_params.Add(rowid);
        }
        const std::string sql =
            "SELECT et.entry_rowid, t.name FROM memory_entry_tags et JOIN memory_tags t "
            "ON t.id = et.tag_id WHERE et.entry_rowid IN (" + rowids + ") ORDER BY t.name;";
        Stmt stmt(db_, sql.c_str(), "memory: hydrate tags");
        tag_params.Bind(stmt.stmt);
        while (sqlite3_step(stmt.stmt) == SQLITE_ROW) {
            const auto it = id_by_rowid.find(sqlite3_column_int64(stmt.stmt, 0));
            if (it != id_by_rowid.end()) by_id[it->second].tags.push_back(ColText(stmt.stmt, 1));
        }
    }
    std::vector<EntryRow> out;
    out.reserve(ids.size());
    for (const auto& id : ids) {
        auto it = by_id.find(id);
        if (it != by_id.end()) out.push_back(std::move(it->second));
    }
    return out;
}

std::optional<EntryRow> MemoryStore::Get(const std::string& id, const ScopeFilter& filter) {
    std::lock_guard<std::mutex> lock(mu_);
    auto rows = HydrateLocked({id}, filter);
    if (rows.empty()) return std::nullopt;
    return std::move(rows.front());
}

std::vector<EntryRow> MemoryStore::GetMany(const std::vector<std::string>& ids,
                                           const ScopeFilter& filter) {
    std::lock_guard<std::mutex> lock(mu_);
    return HydrateLocked(ids, filter);
}

std::vector<LexicalHit> MemoryStore::Lexical(const std::string& query, const Filters& filters,
                                             int limit) {
    std::vector<LexicalHit> hits;
    std::lock_guard<std::mutex> lock(mu_);
    const std::string match = FtsOrQuery(query);
    if (!match.empty()) {
        Params params;
        const std::string where = FiltersSql(filters, params);
        // Column weights: a hit in the title or description (what the entry
        // is ABOUT) outranks one buried in the body.
        const std::string sql =
            "SELECT e.id, bm25(memory_fts, 4.0, 3.0, 2.0, 1.0) AS r, "
            "snippet(memory_fts, 3, '[', ']', '...', 12) "
            "FROM memory_fts JOIN memory_entries e ON e.rowid_ = memory_fts.rowid "
            "WHERE memory_fts MATCH " + params.Add(match) + " AND " + where +
            " ORDER BY r LIMIT " + params.Add(static_cast<std::int64_t>(limit)) + ";";
        Stmt stmt(db_, sql.c_str(), "memory: lexical search");
        params.Bind(stmt.stmt);
        while (sqlite3_step(stmt.stmt) == SQLITE_ROW) {
            hits.push_back({ColText(stmt.stmt, 0), sqlite3_column_double(stmt.stmt, 1),
                            ColText(stmt.stmt, 2)});
        }
    }
    if (!hits.empty()) return hits;

    const auto terms = SubstringTerms(query);
    if (terms.empty()) return hits;
    Params params;
    const std::string where = FiltersSql(filters, params);
    std::string score;
    for (const auto& term : terms) {
        if (!score.empty()) score += " + ";
        score += "(instr(lower(e.title || ' ' || e.description || ' ' || e.body || ' ' || "
                 "e.tags_text), " + params.Add(term) + ") > 0)";
    }
    const std::string sql = "SELECT e.id, (" + score + ") AS s FROM memory_entries e WHERE " +
                            where + " AND s > 0 ORDER BY s DESC, e.updated_ms DESC LIMIT " +
                            params.Add(static_cast<std::int64_t>(limit)) + ";";
    Stmt stmt(db_, sql.c_str(), "memory: substring search");
    params.Bind(stmt.stmt);
    while (sqlite3_step(stmt.stmt) == SQLITE_ROW) {
        hits.push_back({ColText(stmt.stmt, 0), sqlite3_column_double(stmt.stmt, 1), {}});
    }
    return hits;
}

std::vector<std::string> MemoryStore::Browse(const Filters& filters, int limit) {
    std::lock_guard<std::mutex> lock(mu_);
    Params params;
    const std::string sql = "SELECT e.id FROM memory_entries e WHERE " + FiltersSql(filters, params) +
                            " ORDER BY e.pinned DESC, e.updated_ms DESC, e.rowid_ DESC LIMIT " +
                            params.Add(static_cast<std::int64_t>(limit)) + ";";
    Stmt stmt(db_, sql.c_str(), "memory: browse");
    params.Bind(stmt.stmt);
    std::vector<std::string> ids;
    while (sqlite3_step(stmt.stmt) == SQLITE_ROW) ids.push_back(ColText(stmt.stmt, 0));
    return ids;
}

std::optional<std::unordered_set<std::string>> MemoryStore::AllowedIds(const Filters& filters) {
    if (filters.tags.empty()) return std::nullopt;
    std::lock_guard<std::mutex> lock(mu_);
    Params params;
    const std::string sql =
        "SELECT e.id FROM memory_entries e WHERE " + FiltersSql(filters, params) + ";";
    Stmt stmt(db_, sql.c_str(), "memory: allowed ids");
    params.Bind(stmt.stmt);
    std::unordered_set<std::string> ids;
    while (sqlite3_step(stmt.stmt) == SQLITE_ROW) ids.insert(ColText(stmt.stmt, 0));
    return ids;
}

// ----------------------------------------------------------------- vectors

void MemoryStore::EnsureVectorCacheLocked(const std::string& signature) {
    if (cache_loaded_ && cache_signature_ == signature) return;
    cache_.clear();
    cache_signature_ = signature;
    cache_loaded_ = true;
    Stmt stmt(db_, "SELECT id, scope, project_root, embedding FROM memory_entries "
                   "WHERE embed_status = 1 AND signature = ?1 AND embedding IS NOT NULL;",
              "memory: load vectors");
    BindText(stmt.stmt, 1, signature);
    while (sqlite3_step(stmt.stmt) == SQLITE_ROW) {
        CachedVector vector;
        vector.scope = sqlite3_column_int(stmt.stmt, 1);
        vector.project_root = ColText(stmt.stmt, 2);
        vector.unit = DecodeBlob(sqlite3_column_blob(stmt.stmt, 3), sqlite3_column_bytes(stmt.stmt, 3));
        if (Normalize(vector.unit)) cache_.emplace(ColText(stmt.stmt, 0), std::move(vector));
    }
}

std::vector<VectorHit> MemoryStore::Nearest(const std::vector<float>& query,
                                            const std::string& signature,
                                            const ScopeFilter& filter, int k,
                                            const std::unordered_set<std::string>* allowed) {
    std::vector<float> unit = query;
    if (!Normalize(unit) || k <= 0) return {};
    std::lock_guard<std::mutex> lock(mu_);
    EnsureVectorCacheLocked(signature);
    std::vector<VectorHit> hits;
    for (const auto& [id, vector] : cache_) {
        if (vector.unit.size() != unit.size()) continue;
        if (!Visible(filter, vector.scope, vector.project_root)) continue;
        if (allowed != nullptr && allowed->count(id) == 0) continue;
        double dot = 0;
        for (std::size_t i = 0; i < unit.size(); ++i) dot += static_cast<double>(unit[i]) * vector.unit[i];
        hits.push_back({id, dot});
    }
    const auto top = std::min<std::size_t>(hits.size(), static_cast<std::size_t>(k));
    std::partial_sort(hits.begin(), hits.begin() + static_cast<std::ptrdiff_t>(top), hits.end(),
                      [](const VectorHit& a, const VectorHit& b) {
                          return a.score != b.score ? a.score > b.score : a.id > b.id;
                      });
    hits.resize(top);
    return hits;
}

// ------------------------------------------------------- embedding pipeline

std::vector<PendingEmbedRow> MemoryStore::PendingEmbeddings(int limit) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt stmt(db_, "SELECT id, title, description, body FROM memory_entries "
                   "WHERE embed_status = 0 ORDER BY created_ms LIMIT ?1;",
              "memory: pending embeddings");
    sqlite3_bind_int(stmt.stmt, 1, std::max(limit, 0));
    std::vector<PendingEmbedRow> out;
    while (sqlite3_step(stmt.stmt) == SQLITE_ROW) {
        out.push_back({ColText(stmt.stmt, 0),
                       EmbedText(ColText(stmt.stmt, 1), ColText(stmt.stmt, 2), ColText(stmt.stmt, 3))});
    }
    return out;
}

void MemoryStore::StoreEmbedding(const std::string& id, const Embedding& embedding,
                                 const std::string& signature) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt stmt(db_, "UPDATE memory_entries SET embedding = ?2, embed_status = 1, dim = ?3, "
                   "signature = ?4, embed_error = NULL WHERE id = ?1;",
              "memory: store embedding");
    BindText(stmt.stmt, 1, id);
    sqlite3_bind_blob(stmt.stmt, 2, embedding.values.data(),
                      static_cast<int>(embedding.values.size() * sizeof(float)), SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt.stmt, 3, embedding.dim);
    BindText(stmt.stmt, 4, signature);
    StepDone(db_, stmt.stmt, "memory: store embedding");
    if (sqlite3_changes(db_) == 0 || !cache_loaded_ || cache_signature_ != signature) return;

    Stmt row(db_, "SELECT scope, project_root FROM memory_entries WHERE id = ?1;",
             "memory: cache embedding");
    BindText(row.stmt, 1, id);
    if (sqlite3_step(row.stmt) != SQLITE_ROW) return;
    CachedVector vector;
    vector.scope = sqlite3_column_int(row.stmt, 0);
    vector.project_root = ColText(row.stmt, 1);
    vector.unit = embedding.values;
    if (Normalize(vector.unit)) cache_[id] = std::move(vector);
}

void MemoryStore::MarkEmbedFailed(const std::vector<std::string>& ids, const std::string& error) {
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& id : ids) {
        Stmt stmt(db_, "UPDATE memory_entries SET embed_status = 2, embed_error = ?2 WHERE id = ?1;",
                  "memory: mark embed failed");
        BindText(stmt.stmt, 1, id);
        BindText(stmt.stmt, 2, error);
        StepDone(db_, stmt.stmt, "memory: mark embed failed");
    }
}

int MemoryStore::ResetFailedEmbeddings() {
    std::lock_guard<std::mutex> lock(mu_);
    ExecChecked(db_, "UPDATE memory_entries SET embed_status = 0, embed_error = NULL "
                     "WHERE embed_status = 2;",
                "memory: reset failed embeddings");
    return sqlite3_changes(db_);
}

int MemoryStore::ResetStaleSignatures(const std::string& signature) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt stmt(db_, "UPDATE memory_entries SET embed_status = 0, embedding = NULL "
                   "WHERE embed_status IN (1, 2) AND (signature IS NULL OR signature != ?1);",
              "memory: reset stale signatures");
    BindText(stmt.stmt, 1, signature);
    StepDone(db_, stmt.stmt, "memory: reset stale signatures");
    cache_loaded_ = false;
    cache_.clear();
    return sqlite3_changes(db_);
}

// -------------------------------------------------------------------- tags

std::vector<TagInfo> MemoryStore::Tags(int scope, const std::string& project_root) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt stmt(db_, "SELECT t.id, t.name, t.description, (SELECT COUNT(*) FROM memory_entry_tags et "
                   "WHERE et.tag_id = t.id) FROM memory_tags t WHERE t.scope = ?1 AND "
                   "t.project_root = ?2 ORDER BY t.name;",
              "memory: list tags");
    sqlite3_bind_int(stmt.stmt, 1, scope);
    BindText(stmt.stmt, 2, project_root);
    std::vector<TagInfo> out;
    while (sqlite3_step(stmt.stmt) == SQLITE_ROW) {
        TagInfo tag;
        tag.id = sqlite3_column_int64(stmt.stmt, 0);
        tag.scope = scope;
        tag.project_root = project_root;
        tag.name = ColText(stmt.stmt, 1);
        tag.description = ColText(stmt.stmt, 2);
        tag.count = sqlite3_column_int(stmt.stmt, 3);
        out.push_back(std::move(tag));
    }
    return out;
}

std::optional<TagInfo> MemoryStore::FindTag(int scope, const std::string& project_root,
                                            const std::string& name) {
    const std::string key = TagMatchKey(NormalizeTagName(name));
    if (key.empty()) return std::nullopt;
    std::lock_guard<std::mutex> lock(mu_);
    Stmt stmt(db_, "SELECT id, name, description FROM memory_tags WHERE scope = ?1 AND "
                   "project_root = ?2 AND match_key = ?3;",
              "memory: find tag");
    sqlite3_bind_int(stmt.stmt, 1, scope);
    BindText(stmt.stmt, 2, project_root);
    BindText(stmt.stmt, 3, key);
    if (sqlite3_step(stmt.stmt) != SQLITE_ROW) return std::nullopt;
    TagInfo tag;
    tag.id = sqlite3_column_int64(stmt.stmt, 0);
    tag.scope = scope;
    tag.project_root = project_root;
    tag.name = ColText(stmt.stmt, 1);
    tag.description = ColText(stmt.stmt, 2);
    return tag;
}

int MemoryStore::TagCount(int scope, const std::string& project_root) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt stmt(db_, "SELECT COUNT(*) FROM memory_tags WHERE scope = ?1 AND project_root = ?2;",
              "memory: count tags");
    sqlite3_bind_int(stmt.stmt, 1, scope);
    BindText(stmt.stmt, 2, project_root);
    return sqlite3_step(stmt.stmt) == SQLITE_ROW ? sqlite3_column_int(stmt.stmt, 0) : 0;
}

std::int64_t MemoryStore::CreateTag(int scope, const std::string& project_root,
                                    const std::string& name, const std::string& description) {
    std::lock_guard<std::mutex> lock(mu_);
    return CreateTagLocked(db_, scope, project_root, name, description);
}

void MemoryStore::RefreshTagsTextLocked(std::int64_t rowid) {
    Stmt stmt(db_, "UPDATE memory_entries SET tags_text = coalesce((SELECT group_concat(name, ' ') "
                   "FROM (SELECT t.name FROM memory_entry_tags et JOIN memory_tags t ON "
                   "t.id = et.tag_id WHERE et.entry_rowid = ?1 ORDER BY t.name)), '') "
                   "WHERE rowid_ = ?1;",
              "memory: refresh tags_text");
    sqlite3_bind_int64(stmt.stmt, 1, rowid);
    StepDone(db_, stmt.stmt, "memory: refresh tags_text");
}

void MemoryStore::SetEntryTags(const std::string& entry_id, const std::vector<std::int64_t>& tag_ids) {
    std::lock_guard<std::mutex> lock(mu_);
    InTransaction(db_, "set tags", [&] {
        std::int64_t rowid = 0;
        {
            Stmt stmt(db_, "SELECT rowid_ FROM memory_entries WHERE id = ?1;", "memory: entry rowid");
            BindText(stmt.stmt, 1, entry_id);
            if (sqlite3_step(stmt.stmt) != SQLITE_ROW) return;  // deleted meanwhile
            rowid = sqlite3_column_int64(stmt.stmt, 0);
        }
        {
            Stmt stmt(db_, "DELETE FROM memory_entry_tags WHERE entry_rowid = ?1;",
                      "memory: clear entry tags");
            sqlite3_bind_int64(stmt.stmt, 1, rowid);
            StepDone(db_, stmt.stmt, "memory: clear entry tags");
        }
        for (const auto tag_id : tag_ids) {
            Stmt stmt(db_, "INSERT OR IGNORE INTO memory_entry_tags (entry_rowid, tag_id) "
                           "VALUES (?1, ?2);",
                      "memory: bind tag");
            sqlite3_bind_int64(stmt.stmt, 1, rowid);
            sqlite3_bind_int64(stmt.stmt, 2, tag_id);
            StepDone(db_, stmt.stmt, "memory: bind tag");
        }
        RefreshTagsTextLocked(rowid);
        Stmt stmt(db_, "UPDATE memory_entries SET tag_status = 1 WHERE rowid_ = ?1;",
                  "memory: mark tagged");
        sqlite3_bind_int64(stmt.stmt, 1, rowid);
        StepDone(db_, stmt.stmt, "memory: mark tagged");
        DropUnusedTagsLocked();
    });
}

void MemoryStore::SetTagStatus(const std::vector<std::string>& ids, int status) {
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& id : ids) {
        Stmt stmt(db_, "UPDATE memory_entries SET tag_status = ?2 WHERE id = ?1;",
                  "memory: set tag status");
        BindText(stmt.stmt, 1, id);
        sqlite3_bind_int(stmt.stmt, 2, status);
        StepDone(db_, stmt.stmt, "memory: set tag status");
    }
}

std::vector<PendingTagRow> MemoryStore::PendingTagging(int limit) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt stmt(db_, "SELECT id, scope, project_root, ingest_group, title, description, body "
                   "FROM memory_entries WHERE tag_status = 0 ORDER BY created_ms, rowid_ LIMIT ?1;",
              "memory: pending tagging");
    sqlite3_bind_int(stmt.stmt, 1, std::max(limit, 0));
    std::vector<PendingTagRow> out;
    while (sqlite3_step(stmt.stmt) == SQLITE_ROW) {
        PendingTagRow row;
        row.id = ColText(stmt.stmt, 0);
        row.scope = sqlite3_column_int(stmt.stmt, 1);
        row.project_root = ColText(stmt.stmt, 2);
        if (sqlite3_column_type(stmt.stmt, 3) != SQLITE_NULL) row.group_id = ColText(stmt.stmt, 3);
        row.title = ColText(stmt.stmt, 4);
        row.description = ColText(stmt.stmt, 5);
        row.body = ColText(stmt.stmt, 6);
        out.push_back(std::move(row));
    }
    return out;
}

void MemoryStore::DropUnusedTagsLocked() {
    ExecChecked(db_, "DELETE FROM memory_tags WHERE NOT EXISTS (SELECT 1 FROM memory_entry_tags "
                     "WHERE tag_id = memory_tags.id);",
                "memory: drop unused tags");
}

bool MemoryStore::MergeTags(int scope, const std::string& project_root, const std::string& from,
                            const std::string& to) {
    std::lock_guard<std::mutex> lock(mu_);
    const auto from_id = FindTagIdLocked(db_, scope, project_root, TagMatchKey(NormalizeTagName(from)));
    const auto to_id = FindTagIdLocked(db_, scope, project_root, TagMatchKey(NormalizeTagName(to)));
    if (from_id == 0 || to_id == 0 || from_id == to_id) return false;
    InTransaction(db_, "merge tags", [&] {
        std::vector<std::int64_t> rowids;
        {
            Stmt stmt(db_, "SELECT entry_rowid FROM memory_entry_tags WHERE tag_id = ?1;",
                      "memory: merge members");
            sqlite3_bind_int64(stmt.stmt, 1, from_id);
            while (sqlite3_step(stmt.stmt) == SQLITE_ROW) rowids.push_back(sqlite3_column_int64(stmt.stmt, 0));
        }
        for (const auto rowid : rowids) {
            Stmt move(db_, "INSERT OR IGNORE INTO memory_entry_tags (entry_rowid, tag_id) "
                           "VALUES (?1, ?2);",
                      "memory: merge move");
            sqlite3_bind_int64(move.stmt, 1, rowid);
            sqlite3_bind_int64(move.stmt, 2, to_id);
            StepDone(db_, move.stmt, "memory: merge move");
        }
        {
            Stmt drop(db_, "DELETE FROM memory_tags WHERE id = ?1;", "memory: merge drop");
            sqlite3_bind_int64(drop.stmt, 1, from_id);
            StepDone(db_, drop.stmt, "memory: merge drop");
        }
        for (const auto rowid : rowids) RefreshTagsTextLocked(rowid);
    });
    return true;
}

bool MemoryStore::RenameTag(int scope, const std::string& project_root, const std::string& from,
                            const std::string& to) {
    const std::string normalized = NormalizeTagName(to);
    const std::string key = TagMatchKey(normalized);
    if (key.empty()) return false;
    std::lock_guard<std::mutex> lock(mu_);
    const auto from_id = FindTagIdLocked(db_, scope, project_root, TagMatchKey(NormalizeTagName(from)));
    if (from_id == 0) return false;
    const auto clash = FindTagIdLocked(db_, scope, project_root, key);
    if (clash != 0 && clash != from_id) return false;  // that is a merge, not a rename
    InTransaction(db_, "rename tag", [&] {
        Stmt stmt(db_, "UPDATE memory_tags SET name = ?2, match_key = ?3 WHERE id = ?1;",
                  "memory: rename tag");
        sqlite3_bind_int64(stmt.stmt, 1, from_id);
        BindText(stmt.stmt, 2, normalized);
        BindText(stmt.stmt, 3, key);
        StepDone(db_, stmt.stmt, "memory: rename tag");
        std::vector<std::int64_t> rowids;
        {
            Stmt members(db_, "SELECT entry_rowid FROM memory_entry_tags WHERE tag_id = ?1;",
                         "memory: rename members");
            sqlite3_bind_int64(members.stmt, 1, from_id);
            while (sqlite3_step(members.stmt) == SQLITE_ROW) {
                rowids.push_back(sqlite3_column_int64(members.stmt, 0));
            }
        }
        for (const auto rowid : rowids) RefreshTagsTextLocked(rowid);
    });
    return true;
}

// ---------------------------------------------------------- index + stats

std::vector<EntryRow> MemoryStore::Pinned(const ScopeFilter& filter) {
    std::lock_guard<std::mutex> lock(mu_);
    Params params;
    const std::string sql = "SELECT e.id FROM memory_entries e WHERE e.pinned = 1 AND " +
                            ScopeSql(filter, params) + " ORDER BY e.scope DESC, e.updated_ms DESC;";
    Stmt stmt(db_, sql.c_str(), "memory: pinned");
    params.Bind(stmt.stmt);
    std::vector<std::string> ids;
    while (sqlite3_step(stmt.stmt) == SQLITE_ROW) ids.push_back(ColText(stmt.stmt, 0));
    return HydrateLocked(ids, filter);
}

std::vector<EntryRow> MemoryStore::Recent(const ScopeFilter& filter, int limit) {
    std::lock_guard<std::mutex> lock(mu_);
    Params params;
    const std::string sql = "SELECT e.id FROM memory_entries e WHERE e.pinned = 0 AND " +
                            ScopeSql(filter, params) +
                            " ORDER BY e.updated_ms DESC, e.rowid_ DESC LIMIT " +
                            params.Add(static_cast<std::int64_t>(limit)) + ";";
    Stmt stmt(db_, sql.c_str(), "memory: recent");
    params.Bind(stmt.stmt);
    std::vector<std::string> ids;
    while (sqlite3_step(stmt.stmt) == SQLITE_ROW) ids.push_back(ColText(stmt.stmt, 0));
    return HydrateLocked(ids, filter);
}

StoreStats MemoryStore::Stats(const ScopeFilter& filter) {
    std::lock_guard<std::mutex> lock(mu_);
    StoreStats stats;
    auto run = [&](const std::string& select, const std::string& tail, auto&& row) {
        Params params;
        const std::string sql = select + " FROM memory_entries e WHERE " + ScopeSql(filter, params) + tail;
        Stmt stmt(db_, sql.c_str(), "memory: stats");
        params.Bind(stmt.stmt);
        while (sqlite3_step(stmt.stmt) == SQLITE_ROW) row(stmt.stmt);
    };
    run("SELECT e.scope, COUNT(*)", " GROUP BY e.scope;", [&](sqlite3_stmt* s) {
        stats.entries_by_scope[sqlite3_column_int(s, 0) == kScopeProject ? "project" : "global"] =
            sqlite3_column_int(s, 1);
    });
    run("SELECT e.origin, COUNT(*)", " GROUP BY e.origin;", [&](sqlite3_stmt* s) {
        stats.by_origin[ColText(s, 0)] = sqlite3_column_int(s, 1);
    });
    run("SELECT SUM(e.pinned), SUM(e.embed_status = 0), SUM(e.embed_status = 2), "
        "SUM(e.tag_status = 0)",
        ";", [&](sqlite3_stmt* s) {
            stats.pinned = sqlite3_column_int(s, 0);
            stats.pending_embed = sqlite3_column_int(s, 1);
            stats.failed_embed = sqlite3_column_int(s, 2);
            stats.pending_tagging = sqlite3_column_int(s, 3);
        });
    {
        Stmt stmt(db_, "SELECT COUNT(*) FROM memory_tags WHERE (scope = 0 AND ?1) OR "
                       "(scope = 1 AND ?2 AND project_root = ?3);",
                  "memory: stats tags");
        sqlite3_bind_int(stmt.stmt, 1, filter.include_global ? 1 : 0);
        sqlite3_bind_int(stmt.stmt, 2, filter.include_project ? 1 : 0);
        BindText(stmt.stmt, 3, filter.project_root);
        if (sqlite3_step(stmt.stmt) == SQLITE_ROW) stats.tags = sqlite3_column_int(stmt.stmt, 0);
    }
    return stats;
}

}  // namespace minimal::memory
