#include "memory_store.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

#include <sqlite3.h>

#include "minimal/common/file_lock.hpp"
#include "minimal/common/sqlite_util.hpp"
#include "minimal/session/ulid.hpp"

namespace minimal::memory {
namespace {

using common::ExecChecked;
using common::ExclusiveFileLock;
using common::Stmt;
using common::ThrowSqliteError;

constexpr int kSchemaVersion = 1;

// text is the FTS-indexed column; project_root indexes scope narrowing without
// a join. External content (content=) keeps one copy of the text; the triggers
// below keep the index in sync -- the standard recipe, and the reason every
// text write goes through Insert/Delete/SetText rather than raw SQL.
constexpr const char* kCreateSql = R"sql(
CREATE TABLE memory_entries (
    rowid_       INTEGER PRIMARY KEY,
    id           TEXT UNIQUE NOT NULL,
    scope        INTEGER NOT NULL,
    project_root TEXT NOT NULL DEFAULT '',
    session_id   TEXT NOT NULL DEFAULT '',
    origin       TEXT NOT NULL DEFAULT 'model',
    origin_ref   TEXT,
    ingest_group TEXT,
    created_ms   INTEGER NOT NULL,
    text         TEXT NOT NULL,
    summary      TEXT,
    topic_id     INTEGER REFERENCES memory_topics(id),
    embedding    BLOB,
    embed_status INTEGER NOT NULL DEFAULT 0,
    signature    TEXT,
    dim          INTEGER,
    embed_error  TEXT
);
CREATE INDEX memory_entries_scope ON memory_entries(scope, project_root);
CREATE INDEX memory_entries_pending ON memory_entries(embed_status) WHERE embed_status = 0;
CREATE INDEX memory_entries_by_group ON memory_entries(ingest_group);

CREATE TABLE memory_topics (
    id           INTEGER PRIMARY KEY,
    scope        INTEGER NOT NULL,
    project_root TEXT NOT NULL DEFAULT '',
    name         TEXT NOT NULL,
    created_ms   INTEGER NOT NULL,
    last_used_ms INTEGER NOT NULL DEFAULT 0,
    UNIQUE(scope, project_root, name)
);

CREATE TABLE memory_ingest_groups (
    id           TEXT PRIMARY KEY,
    scope        INTEGER NOT NULL,
    project_root TEXT NOT NULL DEFAULT '',
    origin_ref   TEXT NOT NULL,
    chunk_count  INTEGER NOT NULL,
    created_ms   INTEGER NOT NULL
);

CREATE TABLE memory_meta (key TEXT PRIMARY KEY, value TEXT NOT NULL);

CREATE VIRTUAL TABLE memory_fts USING fts5(
    text, project_root, content='memory_entries', content_rowid='rowid_',
    tokenize='trigram'
);
CREATE TRIGGER memory_fts_insert AFTER INSERT ON memory_entries BEGIN
    INSERT INTO memory_fts(rowid, text, project_root)
        VALUES (new.rowid_, new.text, new.project_root);
END;
CREATE TRIGGER memory_fts_delete AFTER DELETE ON memory_entries BEGIN
    INSERT INTO memory_fts(memory_fts, rowid, text, project_root)
        VALUES ('delete', old.rowid_, old.text, old.project_root);
END;
)sql";

// 128 code points, not 128 bytes: a byte cut could split a UTF-8 sequence and
// render U+FFFD in the one surface the model reads (session-titler's rule).
std::string TruncateCodePoints(std::string_view text, std::size_t max_chars) {
    std::size_t chars = 0, i = 0;
    while (i < text.size() && chars < max_chars) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        std::size_t len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2
                          : (c & 0xF0) == 0xE0 ? 3
                          : (c & 0xF8) == 0xF0 ? 4 : 1;
        if (i + len > text.size()) len = text.size() - i;
        i += len;
        ++chars;
    }
    return std::string(text.substr(0, i));
}

std::vector<float> DecodeBlob(const void* data, int bytes) {
    std::vector<float> out;
    if (data != nullptr && bytes > 0) {
        out.resize(static_cast<std::size_t>(bytes) / sizeof(float));
        std::memcpy(out.data(), data, out.size() * sizeof(float));
    }
    return out;
}

// FTS5 MATCH syntax is not model-controlled (the query field is quoted), but a
// stray quote inside would still break the expression -- escape by doubling.
std::string FtsQuote(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"') out += "\"\"";
        out += c;
    }
    out += "\"";
    return out;
}

}  // namespace

double Cosine(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size() || a.empty()) return 0.0;
    double dot = 0, na = 0, nb = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        dot += static_cast<double>(a[i]) * b[i];
        na += static_cast<double>(a[i]) * a[i];
        nb += static_cast<double>(b[i]) * b[i];
    }
    if (na == 0 || nb == 0) return 0.0;
    return dot / (std::sqrt(na) * std::sqrt(nb));
}

MemoryStore::MemoryStore(const std::filesystem::path& db_path) : path_(db_path) {
    std::filesystem::create_directories(db_path.parent_path());
    lock_ = std::make_unique<common::ExclusiveFileLock>(db_path.string() + ".lock");
    if (sqlite3_open(db_path.string().c_str(), &db_) != SQLITE_OK) {
        const std::string message = db_ != nullptr ? sqlite3_errmsg(db_) : "unknown";
        sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("memory: cannot open store " + db_path.string() + ": " + message);
    }
    ExecChecked(db_, "PRAGMA journal_mode=WAL;", "memory: journal mode");
    ExecChecked(db_, "PRAGMA foreign_keys=ON;", "memory: foreign keys");
    Migrate();
    // embed_status=3 (in-flight) only exists for the duration of one
    // synchronous ClaimPendingEmbeddings-to-StoreEmbedding/MarkEmbedFailed
    // call; nothing legitimately holds it across a process restart. A row
    // still at 3 here belongs to a process that died mid-embed -- reset it
    // to pending so it isn't orphaned forever (invisible to both search,
    // which needs status 1, and the next drain, which claims status 0).
    ExecChecked(db_, "UPDATE memory_entries SET embed_status = 0 WHERE embed_status = 3;",
                "memory: reset stale in-flight rows");
}

MemoryStore::~MemoryStore() {
    if (db_ != nullptr) sqlite3_close(db_);
    // lock_ declared before db_, destroyed after: never an open database under
    // a lock file that no longer names this process.
}

void MemoryStore::Migrate() {
    ExecChecked(db_, "BEGIN IMMEDIATE;", "memory: begin migration");
    try {
        int current = 0;
        {
            Stmt stmt(db_, "PRAGMA user_version;", "memory: read schema version");
            if (sqlite3_step(stmt.stmt) == SQLITE_ROW) current = sqlite3_column_int(stmt.stmt, 0);
        }
        if (current > kSchemaVersion) {
            throw std::runtime_error("memory: schema version " + std::to_string(current) +
                                     " is newer than this binary understands; refusing to open");
        }
        if (current < 1) {
            ExecChecked(db_, kCreateSql, "memory: migration v1");
        }
        ExecChecked(db_, ("PRAGMA user_version = " + std::to_string(kSchemaVersion) + ";").c_str(),
                    "memory: record schema version");
        ExecChecked(db_, "COMMIT;", "memory: commit migration");
    } catch (...) {
        common::BestEffortRollback(db_);
        throw;
    }
}

std::string MemoryStore::Iso8601(std::int64_t ms) const {
    const std::time_t seconds = static_cast<std::time_t>(ms / 1000);
    std::tm tm{};
    gmtime_r(&seconds, &tm);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buffer;
}

std::string MemoryStore::Insert(const NewEntry& entry) {
    const std::string id = minimal::session::GenerateUlid();
    const std::int64_t now = common::NowMillis();
    std::lock_guard<std::mutex> lock(mu_);
    {
        Stmt stmt(db_,
                  "INSERT INTO memory_entries (id, scope, project_root, session_id, origin, "
                  "origin_ref, ingest_group, created_ms, text) VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9);",
                  "memory: insert entry");
        sqlite3_bind_text(stmt.stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt.stmt, 2, entry.scope);
        sqlite3_bind_text(stmt.stmt, 3, entry.project_root.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt.stmt, 4, entry.session_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt.stmt, 5, entry.origin.c_str(), -1, SQLITE_TRANSIENT);
        if (entry.origin_ref.empty()) {
            sqlite3_bind_null(stmt.stmt, 6);
        } else {
            sqlite3_bind_text(stmt.stmt, 6, entry.origin_ref.c_str(), -1, SQLITE_TRANSIENT);
        }
        if (entry.group_id) {
            sqlite3_bind_text(stmt.stmt, 7, entry.group_id->c_str(), -1, SQLITE_TRANSIENT);
        } else {
            sqlite3_bind_null(stmt.stmt, 7);
        }
        sqlite3_bind_int64(stmt.stmt, 8, now);
        sqlite3_bind_text(stmt.stmt, 9, entry.text.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt.stmt) != SQLITE_DONE) {
            common::ThrowSqliteError(db_, "memory: insert entry");
        }
    }
    return id;
}

MemoryStore::IngestResult MemoryStore::Ingest(const NewEntry& first,
                                              const std::vector<NewEntry>& chunks,
                                              const std::string& origin_ref) {
    const std::string group_id = "ig" + minimal::session::GenerateUlid();
    const std::int64_t now = common::NowMillis();
    IngestResult result;

    std::lock_guard<std::mutex> lock(mu_);
    ExecChecked(db_, "BEGIN IMMEDIATE;", "memory: begin ingest");
    try {
        // Replace semantics: the path is the stable identity, so a re-ingest
        // deletes the previous rows of this origin_ref (and the group row)
        // before inserting -- refinement, never accumulation.
        {
            Stmt stmt(db_, "SELECT id FROM memory_entries WHERE ingest_group IN "
                           "(SELECT id FROM memory_ingest_groups WHERE origin_ref = ?1 "
                           "AND scope = ?2 AND project_root = ?3);",
                      "memory: find previous ingest rows");
            sqlite3_bind_text(stmt.stmt, 1, origin_ref.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt.stmt, 2, first.scope);
            sqlite3_bind_text(stmt.stmt, 3, first.project_root.c_str(), -1, SQLITE_TRANSIENT);
            std::vector<std::string> doomed;
            while (sqlite3_step(stmt.stmt) == SQLITE_ROW) {
                doomed.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(stmt.stmt, 0)));
            }
            result.replaced = static_cast<int>(doomed.size());
            for (const auto& id : doomed) {
                Stmt del(db_, "DELETE FROM memory_entries WHERE id = ?1;",
                         "memory: delete previous ingest row");
                sqlite3_bind_text(del.stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(del.stmt) != SQLITE_DONE) {
                    common::ThrowSqliteError(db_, "memory: delete previous ingest row");
                }
            }
        }
        {
            Stmt stmt(db_, "DELETE FROM memory_ingest_groups WHERE origin_ref = ?1 "
                           "AND scope = ?2 AND project_root = ?3;",
                      "memory: delete previous ingest group");
            sqlite3_bind_text(stmt.stmt, 1, origin_ref.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt.stmt, 2, first.scope);
            sqlite3_bind_text(stmt.stmt, 3, first.project_root.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt.stmt) != SQLITE_DONE) {
                common::ThrowSqliteError(db_, "memory: delete previous ingest group");
            }
        }
        {
            Stmt stmt(db_, "INSERT INTO memory_ingest_groups (id, scope, project_root, origin_ref, "
                           "chunk_count, created_ms) VALUES (?1,?2,?3,?4,?5,?6);",
                      "memory: insert ingest group");
            sqlite3_bind_text(stmt.stmt, 1, group_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt.stmt, 2, first.scope);
            sqlite3_bind_text(stmt.stmt, 3, first.project_root.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt.stmt, 4, origin_ref.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt.stmt, 5, static_cast<int>(chunks.size() + 1));
            sqlite3_bind_int64(stmt.stmt, 6, now);
            if (sqlite3_step(stmt.stmt) != SQLITE_DONE) {
                common::ThrowSqliteError(db_, "memory: insert ingest group");
            }
        }

        auto insert_one = [&](const NewEntry& entry) {
            const std::string id = minimal::session::GenerateUlid();
            Stmt stmt(db_,
                      "INSERT INTO memory_entries (id, scope, project_root, session_id, origin, "
                      "origin_ref, ingest_group, created_ms, text) VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9);",
                      "memory: insert ingest chunk");
            sqlite3_bind_text(stmt.stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt.stmt, 2, entry.scope);
            sqlite3_bind_text(stmt.stmt, 3, entry.project_root.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt.stmt, 4, entry.session_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt.stmt, 5, entry.origin.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt.stmt, 6, origin_ref.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt.stmt, 7, group_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(stmt.stmt, 8, now);
            sqlite3_bind_text(stmt.stmt, 9, entry.text.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt.stmt) != SQLITE_DONE) {
                common::ThrowSqliteError(db_, "memory: insert ingest chunk");
            }
            result.ids.push_back(id);
        };
        insert_one(first);
        for (const auto& chunk : chunks) insert_one(chunk);

        ExecChecked(db_, "COMMIT;", "memory: commit ingest");
    } catch (...) {
        common::BestEffortRollback(db_);
        throw;
    }
    result.group_id = group_id;
    return result;
}

bool MemoryStore::Delete(const std::string& id) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt stmt(db_, "DELETE FROM memory_entries WHERE id = ?1;", "memory: delete entry");
    sqlite3_bind_text(stmt.stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt.stmt) != SQLITE_DONE) {
        common::ThrowSqliteError(db_, "memory: delete entry");
    }
    return sqlite3_changes(db_) > 0;
}

int MemoryStore::PurgeByOriginRef(const std::string& origin_ref, int scope,
                                  const std::string& project_root) {
    std::lock_guard<std::mutex> lock(mu_);
    int removed = 0;
    ExecChecked(db_, "BEGIN IMMEDIATE;", "memory: begin purge");
    try {
        {
            Stmt stmt(db_, "DELETE FROM memory_entries WHERE origin_ref = ?1 AND scope = ?2 "
                           "AND project_root = ?3;",
                      "memory: purge entries");
            sqlite3_bind_text(stmt.stmt, 1, origin_ref.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt.stmt, 2, scope);
            sqlite3_bind_text(stmt.stmt, 3, project_root.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt.stmt) != SQLITE_DONE) {
                common::ThrowSqliteError(db_, "memory: purge entries");
            }
            removed = sqlite3_changes(db_);
        }
        {
            Stmt stmt(db_, "DELETE FROM memory_ingest_groups WHERE origin_ref = ?1 "
                           "AND scope = ?2 AND project_root = ?3;",
                      "memory: purge group");
            sqlite3_bind_text(stmt.stmt, 1, origin_ref.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt.stmt, 2, scope);
            sqlite3_bind_text(stmt.stmt, 3, project_root.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt.stmt) != SQLITE_DONE) {
                common::ThrowSqliteError(db_, "memory: purge group");
            }
        }
        ExecChecked(db_, "COMMIT;", "memory: commit purge");
    } catch (...) {
        common::BestEffortRollback(db_);
        throw;
    }
    return removed;
}

void MemoryStore::SetSummary(const std::string& id, const std::string& summary) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt stmt(db_, "UPDATE memory_entries SET summary = ?2 WHERE id = ?1;",
              "memory: set summary");
    sqlite3_bind_text(stmt.stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.stmt, 2, summary.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt.stmt) != SQLITE_DONE) {
        common::ThrowSqliteError(db_, "memory: set summary");
    }
}

bool MemoryStore::BindTopic(const std::string& entry_id, int scope,
                            const std::string& project_root, const std::string& name,
                            bool create_if_missing) {
    std::lock_guard<std::mutex> lock(mu_);
    const std::int64_t now = common::NowMillis();
    std::int64_t topic_id = 0;

    // Case-insensitive match against the scope's existing topics.
    {
        Stmt stmt(db_, "SELECT id FROM memory_topics WHERE scope = ?1 AND project_root = ?2 "
                       "AND name = ?3 COLLATE NOCASE;",
                  "memory: find topic");
        sqlite3_bind_int(stmt.stmt, 1, scope);
        sqlite3_bind_text(stmt.stmt, 2, project_root.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt.stmt, 3, name.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt.stmt) == SQLITE_ROW) {
            topic_id = sqlite3_column_int64(stmt.stmt, 0);
        }
    }

    if (topic_id == 0) {
        if (!create_if_missing) return false;

        // The cap: never trim, never displace -- at cap a new topic is
        // refused and the entry stays unfiled.
        {
            Stmt stmt(db_, "SELECT COUNT(*) FROM memory_topics WHERE scope = ?1 "
                           "AND project_root = ?2;",
                      "memory: count topics");
            sqlite3_bind_int(stmt.stmt, 1, scope);
            sqlite3_bind_text(stmt.stmt, 2, project_root.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt.stmt) != SQLITE_ROW) {
                common::ThrowSqliteError(db_, "memory: count topics");
            }
            if (sqlite3_column_int64(stmt.stmt, 0) >= 4096) return false;
        }
        {
            Stmt stmt(db_, "INSERT INTO memory_topics (scope, project_root, name, created_ms, "
                           "last_used_ms) VALUES (?1,?2,?3,?4,?4);",
                      "memory: create topic");
            sqlite3_bind_int(stmt.stmt, 1, scope);
            sqlite3_bind_text(stmt.stmt, 2, project_root.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt.stmt, 3, name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(stmt.stmt, 4, now);
            if (sqlite3_step(stmt.stmt) != SQLITE_DONE) {
                common::ThrowSqliteError(db_, "memory: create topic");
            }
            topic_id = sqlite3_last_insert_rowid(db_);
        }
    }

    {
        Stmt stmt(db_, "UPDATE memory_topics SET last_used_ms = ?2 WHERE id = ?1;",
                  "memory: touch topic");
        sqlite3_bind_int64(stmt.stmt, 1, topic_id);
        sqlite3_bind_int64(stmt.stmt, 2, now);
        if (sqlite3_step(stmt.stmt) != SQLITE_DONE) {
            common::ThrowSqliteError(db_, "memory: touch topic");
        }
    }
    Stmt stmt(db_, "UPDATE memory_entries SET topic_id = ?2 WHERE id = ?1;",
              "memory: bind topic");
    sqlite3_bind_text(stmt.stmt, 1, entry_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt.stmt, 2, topic_id);
    if (sqlite3_step(stmt.stmt) != SQLITE_DONE) {
        common::ThrowSqliteError(db_, "memory: bind topic");
    }
    return true;
}

std::vector<MemoryStore::PendingRow> MemoryStore::ClaimPendingEmbeddings(
    const std::string& signature, int budget) {
    std::vector<PendingRow> out;
    std::lock_guard<std::mutex> lock(mu_);
    ExecChecked(db_, "BEGIN IMMEDIATE;", "memory: begin claim");
    try {
        {
            Stmt stmt(db_, "SELECT id, text FROM memory_entries WHERE embed_status = 0 "
                           "ORDER BY created_ms LIMIT ?1;",
                      "memory: select pending embeddings");
            sqlite3_bind_int(stmt.stmt, 1, budget);
            while (sqlite3_step(stmt.stmt) == SQLITE_ROW) {
                out.push_back({
                    reinterpret_cast<const char*>(sqlite3_column_text(stmt.stmt, 0)),
                    reinterpret_cast<const char*>(sqlite3_column_text(stmt.stmt, 1)),
                });
            }
        }
        for (const auto& row : out) {
            Stmt stmt(db_, "UPDATE memory_entries SET embed_status = 3, signature = ?2 "
                           "WHERE id = ?1;",
                      "memory: mark in-flight");
            sqlite3_bind_text(stmt.stmt, 1, row.id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt.stmt, 2, signature.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt.stmt) != SQLITE_DONE) {
                common::ThrowSqliteError(db_, "memory: mark in-flight");
            }
        }
        // Rows already carried by a DIFFERENT signature (strategy changed)
        // reset to pending: the signature comparison is the reindex trigger.
        {
            Stmt stmt(db_, "UPDATE memory_entries SET embed_status = 0 "
                           "WHERE embed_status IN (1,2) AND signature IS NOT NULL "
                           "AND signature != ?1;",
                      "memory: reset stale-signature rows");
            sqlite3_bind_text(stmt.stmt, 1, signature.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt.stmt) != SQLITE_DONE) {
                common::ThrowSqliteError(db_, "memory: reset stale-signature rows");
            }
        }
        ExecChecked(db_, "COMMIT;", "memory: commit claim");
    } catch (...) {
        common::BestEffortRollback(db_);
        throw;
    }
    return out;
}

void MemoryStore::MarkEmbedFailed(const std::vector<std::string>& ids, const std::string& error) {
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& id : ids) {
        // 2 = failed: a transient backend outage should not wedge rows in
        // in-flight forever, but repeated failure must also not burn the
        // budget every drain -- failed rows retry on the NEXT signature
        // change or explicit /memory reindex, not every drain.
        Stmt stmt(db_, "UPDATE memory_entries SET embed_status = 2, embed_error = ?2 "
                       "WHERE id = ?1;",
                  "memory: mark embed failed");
        sqlite3_bind_text(stmt.stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt.stmt, 2, error.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt.stmt) != SQLITE_DONE) {
            common::ThrowSqliteError(db_, "memory: mark embed failed");
        }
    }
}

void MemoryStore::StoreEmbedding(const std::string& id, const Embedding& embedding) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt stmt(db_, "UPDATE memory_entries SET embedding = ?2, embed_status = 1, dim = ?3 "
                   "WHERE id = ?1;",
              "memory: store embedding");
    sqlite3_bind_text(stmt.stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt.stmt, 2, embedding.values.data(),
                      static_cast<int>(embedding.values.size() * sizeof(float)), SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt.stmt, 3, embedding.dim);
    if (sqlite3_step(stmt.stmt) != SQLITE_DONE) {
        common::ThrowSqliteError(db_, "memory: store embedding");
    }
}

std::vector<MemoryStore::VectorRow> MemoryStore::LoadVectors(const std::string& signature, int scope,
                                                             const std::string& project_root) {
    std::vector<VectorRow> out;
    std::lock_guard<std::mutex> lock(mu_);
    Stmt stmt(db_, "SELECT id, embedding FROM memory_entries WHERE embed_status = 1 "
                   "AND signature = ?1 AND scope = ?2 AND project_root = ?3 AND embedding IS NOT NULL;",
              "memory: load vectors");
    sqlite3_bind_text(stmt.stmt, 1, signature.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt.stmt, 2, scope);
    sqlite3_bind_text(stmt.stmt, 3, project_root.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt.stmt) == SQLITE_ROW) {
        VectorRow row;
        row.id = reinterpret_cast<const char*>(sqlite3_column_text(stmt.stmt, 0));
        row.values = DecodeBlob(sqlite3_column_blob(stmt.stmt, 1), sqlite3_column_bytes(stmt.stmt, 1));
        if (!row.values.empty()) out.push_back(std::move(row));
    }
    return out;
}

int MemoryStore::SetSignatureForAll(const std::string& signature, int dim) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt stmt(db_, "UPDATE memory_entries SET signature = ?1, dim = ?2, embed_status = 0 "
                   "WHERE embed_status = 1 AND signature IS NOT NULL AND signature != ?1;",
              "memory: set signature");
    sqlite3_bind_text(stmt.stmt, 1, signature.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt.stmt, 2, dim);
    if (sqlite3_step(stmt.stmt) != SQLITE_DONE) {
        common::ThrowSqliteError(db_, "memory: set signature");
    }
    return sqlite3_changes(db_);
}

std::vector<SearchHit> MemoryStore::Search(const SearchQuery& query) {
    std::lock_guard<std::mutex> lock(mu_);

    // full_scan: an embedder is available and the caller gave both a sort=sim
    // request and query text to embed. In this case the FTS trigram match
    // would gate cosine reranking down to entries that already lexically
    // overlap the query -- exactly backwards for semantic search, whose whole
    // point is finding differently-worded but conceptually similar entries.
    // So the candidate set drops the FTS gate entirely and the caller
    // (MemorySearchTool) reranks over every matching row by cosine similarity
    // instead, the same unbounded scope LoadVectors() already reads for that
    // rerank -- this does not change the existing cost profile, only which
    // rows are eligible to be found.
    const bool full_scan = query.sort == "sim" && query.has_embeddings && query.query.has_value();

    std::string sql =
        "SELECT e.id, e.text, e.summary, t.name, e.created_ms, e.project_root "
        "FROM memory_entries e LEFT JOIN memory_topics t ON t.id = e.topic_id WHERE "
        "e.scope = ?1 AND e.project_root = ?2 ";
    if (query.query && !full_scan) {
        sql += "AND e.rowid_ IN (SELECT rowid FROM memory_fts WHERE memory_fts MATCH ?3 AND project_root = ?2) ";
    }
    if (!query.keywords.empty()) {
        for (std::size_t i = 0; i < query.keywords.size(); ++i) {
            sql += "AND (instr(lower(e.text), :k" + std::to_string(i) + ") > 0 "
                   "OR instr(lower(coalesce(e.origin_ref, '')), :k" + std::to_string(i) + ") > 0 "
                   "OR instr(lower(coalesce(t.name, '')), :k" + std::to_string(i) + ") > 0) ";
        }
    }
    if (query.topic) sql += "AND t.name = :topic COLLATE NOCASE ";
    if (query.sort == "time") sql += "ORDER BY e.created_ms DESC ";
    else sql += "ORDER BY e.id DESC ";  // sim sort re-ranks below; ULID order is time order
    sql += "LIMIT :limit;";

    Stmt stmt(db_, sql.c_str(), "memory: search");
    int bind = 1;
    sqlite3_bind_int(stmt.stmt, bind++, query.scope);
    sqlite3_bind_text(stmt.stmt, bind++, query.project_root.c_str(), -1, SQLITE_TRANSIENT);
    if (query.query && !full_scan) {
        sqlite3_bind_text(stmt.stmt, bind++, FtsQuote(*query.query).c_str(), -1, SQLITE_TRANSIENT);
    }
    for (std::size_t i = 0; i < query.keywords.size(); ++i) {
        std::string lowered = query.keywords[i];
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const std::string name = ":k" + std::to_string(i);
        const int index = sqlite3_bind_parameter_index(stmt.stmt, name.c_str());
        sqlite3_bind_text(stmt.stmt, index, lowered.c_str(), -1, SQLITE_TRANSIENT);
    }
    if (query.topic) {
        sqlite3_bind_text(stmt.stmt, sqlite3_bind_parameter_index(stmt.stmt, ":topic"),
                          query.topic->c_str(), -1, SQLITE_TRANSIENT);
    }
    // full_scan: no SQL-side cap -- every row in scope is a semantic
    // candidate, matching LoadVectors()'s own unbounded read below. Otherwise
    // (lexical/time paths, or sim-browse with no query text to rerank by):
    // exactly the requested limit, no over-fetch to walk back afterward.
    sqlite3_bind_int(stmt.stmt, sqlite3_bind_parameter_index(stmt.stmt, ":limit"),
                     full_scan ? std::numeric_limits<int>::max() : query.limit);

    std::vector<SearchHit> hits;
    while (sqlite3_step(stmt.stmt) == SQLITE_ROW) {
        SearchHit hit;
        hit.id = reinterpret_cast<const char*>(sqlite3_column_text(stmt.stmt, 0));
        const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt.stmt, 1));
        hit.preview = TruncateCodePoints(text ? text : "", 128);
        if (sqlite3_column_type(stmt.stmt, 2) != SQLITE_NULL) {
            hit.summary = reinterpret_cast<const char*>(sqlite3_column_text(stmt.stmt, 2));
        }
        if (sqlite3_column_type(stmt.stmt, 3) != SQLITE_NULL) {
            hit.topic = reinterpret_cast<const char*>(sqlite3_column_text(stmt.stmt, 3));
        }
        hit.created_ms = sqlite3_column_int64(stmt.stmt, 4);
        hit.project_root = reinterpret_cast<const char*>(sqlite3_column_text(stmt.stmt, 5));
        hits.push_back(std::move(hit));
    }

    if (query.sort == "sim" && query.has_embeddings && !query.query) {
        // No query text to embed; sim over keywords-only is undefined, keep
        // the time-ordered candidates and score 0.
        for (auto& hit : hits) hit.score = 0.0;
    }
    return hits;
}

std::optional<MemoryStore::Entry> MemoryStore::Get(const std::string& id) {
    std::lock_guard<std::mutex> lock(mu_);
    Entry entry;
    {
        Stmt stmt(db_,
                  "SELECT e.id, e.text, e.scope, e.project_root, e.session_id, e.origin, "
                  "e.origin_ref, e.summary, t.name, e.created_ms, e.ingest_group "
                  "FROM memory_entries e LEFT JOIN memory_topics t ON t.id = e.topic_id "
                  "WHERE e.id = ?1;",
                  "memory: get entry");
        sqlite3_bind_text(stmt.stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt.stmt) != SQLITE_ROW) return std::nullopt;
        entry.id = reinterpret_cast<const char*>(sqlite3_column_text(stmt.stmt, 0));
        entry.text = reinterpret_cast<const char*>(sqlite3_column_text(stmt.stmt, 1));
        const int scope = sqlite3_column_int(stmt.stmt, 2);
        entry.scope_name = scope == kScopeProject ? "project" : "global";
        entry.project_root = reinterpret_cast<const char*>(sqlite3_column_text(stmt.stmt, 3));
        entry.session_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt.stmt, 4));
        entry.origin = reinterpret_cast<const char*>(sqlite3_column_text(stmt.stmt, 5));
        if (sqlite3_column_type(stmt.stmt, 6) != SQLITE_NULL) {
            entry.origin_ref = reinterpret_cast<const char*>(sqlite3_column_text(stmt.stmt, 6));
        }
        if (sqlite3_column_type(stmt.stmt, 7) != SQLITE_NULL) {
            entry.summary = reinterpret_cast<const char*>(sqlite3_column_text(stmt.stmt, 7));
        }
        if (sqlite3_column_type(stmt.stmt, 8) != SQLITE_NULL) {
            entry.topic = reinterpret_cast<const char*>(sqlite3_column_text(stmt.stmt, 8));
        }
        entry.created_ms = sqlite3_column_int64(stmt.stmt, 9);
        if (sqlite3_column_type(stmt.stmt, 10) != SQLITE_NULL) {
            entry.group_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt.stmt, 10));
        }
    }
    entry.created_at = Iso8601(entry.created_ms);

    if (entry.group_id) {
        // Sibling pointers: one indexed query over the group.
        Stmt stmt(db_, "SELECT id FROM memory_entries WHERE ingest_group = ?1 ORDER BY rowid_;",
                  "memory: group siblings");
        sqlite3_bind_text(stmt.stmt, 1, entry.group_id->c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(stmt.stmt) == SQLITE_ROW) {
            entry.sibling_ids.emplace_back(
                reinterpret_cast<const char*>(sqlite3_column_text(stmt.stmt, 0)));
        }
        entry.chunk_index = static_cast<int>(
            std::find(entry.sibling_ids.begin(), entry.sibling_ids.end(), entry.id) -
            entry.sibling_ids.begin());
        entry.chunk_count = static_cast<int>(entry.sibling_ids.size());
    }
    return entry;
}

Digest MemoryStore::DigestInfo(const std::string& backend, const std::string& enrichment) {
    Digest digest;
    std::lock_guard<std::mutex> lock(mu_);
    {
        Stmt stmt(db_, "SELECT scope, COUNT(*) FROM memory_entries GROUP BY scope;",
                  "memory: digest counts");
        while (sqlite3_step(stmt.stmt) == SQLITE_ROW) {
            const int scope = sqlite3_column_int(stmt.stmt, 0);
            digest.entries_by_scope[scope == kScopeProject ? "project" : "global"] =
                sqlite3_column_int(stmt.stmt, 1);
        }
    }
    {
        Stmt stmt(db_, "SELECT origin, COUNT(*) FROM memory_entries GROUP BY origin;",
                  "memory: digest origins");
        while (sqlite3_step(stmt.stmt) == SQLITE_ROW) {
            digest.by_origin[reinterpret_cast<const char*>(sqlite3_column_text(stmt.stmt, 0))] =
                sqlite3_column_int(stmt.stmt, 1);
        }
    }
    {
        Stmt stmt(db_, "SELECT t.name, COUNT(*) FROM memory_entries e "
                       "JOIN memory_topics t ON t.id = e.topic_id GROUP BY t.name "
                       "ORDER BY COUNT(*) DESC LIMIT 12;",
                  "memory: digest topics");
        while (sqlite3_step(stmt.stmt) == SQLITE_ROW) {
            digest.top_topics.emplace_back(
                reinterpret_cast<const char*>(sqlite3_column_text(stmt.stmt, 0)),
                sqlite3_column_int(stmt.stmt, 1));
        }
    }
    {
        Stmt stmt(db_, "SELECT COUNT(*) FROM memory_entries WHERE embed_status = 0;",
                  "memory: digest pending");
        if (sqlite3_step(stmt.stmt) == SQLITE_ROW) {
            digest.pending_reindex = sqlite3_column_int(stmt.stmt, 0);
        }
    }
    digest.backend = backend;
    digest.enrichment = enrichment;
    return digest;
}

std::vector<std::string> MemoryStore::TopicNames(int scope, const std::string& project_root,
                                                 int cap) {
    std::vector<std::string> out;
    std::lock_guard<std::mutex> lock(mu_);
    // Recency of use, so the active taxonomy stays in the prompt's view.
    Stmt stmt(db_, "SELECT name FROM memory_topics WHERE scope = ?1 AND project_root = ?2 "
                   "ORDER BY last_used_ms DESC LIMIT ?3;",
              "memory: topic names");
    sqlite3_bind_int(stmt.stmt, 1, scope);
    sqlite3_bind_text(stmt.stmt, 2, project_root.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt.stmt, 3, cap);
    while (sqlite3_step(stmt.stmt) == SQLITE_ROW) {
        out.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(stmt.stmt, 0)));
    }
    return out;
}

std::vector<std::pair<std::string, int>> MemoryStore::AllTopics(int scope,
                                                                const std::string& project_root) {
    std::vector<std::pair<std::string, int>> out;
    std::lock_guard<std::mutex> lock(mu_);
    // All topics INCLUDING zero-entry ones: a still-selectable topic must not
    // vanish from the operator's view (the digest shows counts > 0 instead).
    Stmt stmt(db_, "SELECT t.name, (SELECT COUNT(*) FROM memory_entries e "
                   "WHERE e.topic_id = t.id) FROM memory_topics t "
                   "WHERE t.scope = ?1 AND t.project_root = ?2 ORDER BY t.name;",
              "memory: all topics");
    sqlite3_bind_int(stmt.stmt, 1, scope);
    sqlite3_bind_text(stmt.stmt, 2, project_root.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt.stmt) == SQLITE_ROW) {
        out.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(stmt.stmt, 0)),
                         sqlite3_column_int(stmt.stmt, 1));
    }
    return out;
}

}  // namespace minimal::memory
