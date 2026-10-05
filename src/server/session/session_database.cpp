#include "minimal/session/session_database.hpp"

#include <sqlite3.h>

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <variant>

#include "minimal/common/config_paths.hpp"
#include "minimal/store/file_lock.hpp"
#include "minimal/common/clock.hpp"
#include "minimal/store/sqlite_util.hpp"
#include "minimal/session/message_codec.hpp"
#include "minimal/session/ulid.hpp"

namespace minimal::session {
namespace {

using store::BestEffortRollback;
using store::ExecChecked;
using common::NowMillis;
using store::Stmt;
using store::ThrowSqliteError;

const std::vector<Migration>& DefaultMigrationsImpl() {
    static const std::vector<Migration> migrations = {{
        {1,
         {
             "CREATE TABLE sessions ("
             "  id TEXT PRIMARY KEY,"
             "  parent_session_id TEXT REFERENCES sessions(id) ON DELETE CASCADE,"
             "  name TEXT NOT NULL DEFAULT '',"
             "  project_root TEXT NOT NULL,"
             "  agent_preset TEXT NOT NULL,"
             "  created_at INTEGER NOT NULL,"
             "  updated_at INTEGER NOT NULL"
             ");",
             // UNIQUE(session_id, seq) is the (session_id, seq) index keyset
             // pagination queries against; a separate CREATE INDEX would be a
             // duplicate.
             "CREATE TABLE messages ("
             "  id INTEGER PRIMARY KEY,"
             "  session_id TEXT NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,"
             "  seq INTEGER NOT NULL,"
             "  role TEXT NOT NULL,"
             "  content_json TEXT NOT NULL,"
             "  token_count INTEGER NOT NULL DEFAULT 0,"
             "  created_at INTEGER NOT NULL,"
             "  UNIQUE (session_id, seq)"
              ");",
          }},
        // message_count as a maintained column. ListSessions/GetSession used to
        // pay a correlated COUNT(*) per session row, which is O(total messages)
        // for the page -- exactly the growth pattern the picker's keyset cursor
        // exists to avoid. The count is bumped in the append transaction, so it
        // is exact, and the migration backfills from the rows that exist.
        {2,
         {
             "ALTER TABLE sessions ADD COLUMN message_count INTEGER NOT NULL DEFAULT 0;",
             "UPDATE sessions SET message_count ="
             " (SELECT COUNT(*) FROM messages WHERE messages.session_id = sessions.id);",
         }},
        // The indexes the session picker's keyset cursor has always assumed.
        //
        // §11.8 made the MESSAGE cursor a real seek; the SESSION cursor was
        // still a full scan plus a temp b-tree sort on every page, because
        // nothing indexed (updated_at, id) -- so the predicate filtered rather
        // than sought and paging the picker stayed O(total sessions) per page,
        // which is the exact growth the cursor exists to remove. DESC/ASC in the
        // index matches the ORDER BY so the sort disappears too.
        //
        // The second is the canonical unindexed-foreign-key case: with
        // foreign_keys=ON and ON DELETE CASCADE on the self-reference, every
        // DELETE FROM sessions full-scanned the table looking for children.
        {3,
         {
             "CREATE INDEX IF NOT EXISTS idx_sessions_activity"
             " ON sessions(updated_at DESC, id ASC);",
             "CREATE INDEX IF NOT EXISTS idx_sessions_parent"
             " ON sessions(parent_session_id);",
         }},
        // The persisted compaction checkpoint (see CompactionCheckpoint): the
        // derived VIEW of a session's context, cached so a resume reconstructs
        // [summary + tail] instead of re-summarizing the whole uncompacted
        // transcript -- which for a transcript well beyond the model window
        // could not even fit its own summarization request. A cache of view
        // state, deliberately NOT a message row: `messages` stays the exact
        // record of what was said, scrollback is untouched, and the
        // "compaction never mutates persisted history" rule survives verbatim.
        // ON DELETE CASCADE does the session-deletion cleanup; /clear is the
        // one explicit invalidation (ClearCompactionCheckpoint).
        {4,
         {
             "CREATE TABLE compactions ("
             "  session_id TEXT PRIMARY KEY REFERENCES sessions(id) ON DELETE CASCADE,"
             "  boundary_seq INTEGER NOT NULL,"
             "  summary_content_json TEXT NOT NULL,"
             "  created_at INTEGER NOT NULL"
             ");",
         }},
        // Session traversal (rewind). A discarded row is still stored -- the
        // flag is what keeps redo possible -- but every reader filters it, so
        // scrollback, resume and the compaction boundary all see exactly the
        // active transcript. The index serves both this flag's two query
        // shapes: the mark/unmark scans (session_id, discarded) and the
        // active-only reads that also bound seq. Rows are physically reclaimed
        // by the first append after the rewind (see AppendMessage).
        {5,
         {
             "ALTER TABLE messages ADD COLUMN discarded INTEGER NOT NULL DEFAULT 0;",
             "CREATE INDEX IF NOT EXISTS idx_messages_active"
             " ON messages(session_id, discarded, seq);",
         }},
        // Where a session's name came from: 'explicit' (the user named it at
        // creation), 'default' (the free first-user-text rule), 'observer' (a
        // plugin -- the LLM titler -- set it later). The rename seam checks
        // this so a generated title can never clobber a name the user chose;
        // the in-memory alternative was considered and rejected because the
        // column also records the fact permanently, at the cost of one ALTER.
        // Pre-v6 rows backfill '' -- not known to be explicit, which is the
        // safe reading (see IsExplicitlyNamed).
        {6,
         {
             "ALTER TABLE sessions ADD COLUMN name_source TEXT NOT NULL DEFAULT '';",
         }},
        // Session fork provenance. Deliberately NOT parent_session_id, which
        // means "subagent" everywhere; a fork is a top-level peer. SET NULL on
        // source deletion: provenance is informational, not a lifecycle edge.
        {7,
         {
             "ALTER TABLE sessions ADD COLUMN forked_from_session_id TEXT"
             " REFERENCES sessions(id) ON DELETE SET NULL;",
         }},
        // The session's model override, as the full "<provider>/<model>"
        // reference the user chose (NULL when they never did -- the preset
        // then decides, which is what absence must mean on resume too).
        // Before v8 the override lived only in server memory, so a /model
        // choice was silently dropped by a server restart: resume
        // re-resolved the preset and its default model won. Pre-v8 rows
        // backfill NULL, which reads as "no override" -- the old sessions
        // genuinely never had one.
        {8,
         {
             "ALTER TABLE sessions ADD COLUMN model TEXT;",
         }},
        // Per-message attribution: the agent preset and "<provider>/<model>"
        // that produced an assistant row. The session row's agent_preset and
        // model are only the CURRENT sticky choice -- /agent, /model and
        // per-turn overrides all change what a later row ran on, so without
        // these the transcript could not say who said what. NULL = unknown:
        // every pre-v9 row, and every user/tool-result row by design.
        {9,
         {
             "ALTER TABLE messages ADD COLUMN agent_preset TEXT;",
             "ALTER TABLE messages ADD COLUMN model TEXT;",
         }},
        // The session's subagent-model override, as the full
        // "<provider>/<model>" reference the user chose for spawned children
        // (NULL when they never did -- children then inherit the parent's
        // model, which is what absence must mean on resume too). Follows the
        // v8 pattern exactly; pre-v10 rows backfill NULL, which reads as "no
        // override" -- the old sessions genuinely never had one.
        {10,
         {
             "ALTER TABLE sessions ADD COLUMN subagent_model TEXT;",
         }},
        // Every compaction becomes a record, not a latest-wins checkpoint:
        // scrollback renders each one where it happened (at_seq -- the last
        // active message seq when it ran), with the token figures that make
        // it legible. `active` separates "the checkpoint resume seeds from"
        // (highest boundary among active rows) from "what scrollback shows":
        // /clear deactivates, it no longer deletes. SQLite cannot drop a
        // PRIMARY KEY in place, hence the rebuild. Pre-v11 rows backfill
        // at_seq = boundary_seq (the only position known) and zero figures.
        {11,
         {
             "CREATE TABLE compactions_v11 ("
             "  id INTEGER PRIMARY KEY,"
             "  session_id TEXT NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,"
             "  boundary_seq INTEGER NOT NULL,"
             "  at_seq INTEGER NOT NULL,"
             "  summary_content_json TEXT NOT NULL,"
             "  folded_messages INTEGER NOT NULL DEFAULT 0,"
             "  tokens_before INTEGER NOT NULL DEFAULT 0,"
             "  tokens_after INTEGER NOT NULL DEFAULT 0,"
             "  trigger_kind TEXT NOT NULL DEFAULT '',"
             "  warning TEXT NOT NULL DEFAULT '',"
             "  skipped INTEGER NOT NULL DEFAULT 0,"
             "  active INTEGER NOT NULL DEFAULT 1,"
             "  created_at INTEGER NOT NULL"
             ");",
             "INSERT INTO compactions_v11 (session_id, boundary_seq, at_seq,"
             " summary_content_json, created_at)"
             " SELECT session_id, boundary_seq, boundary_seq, summary_content_json, created_at"
             " FROM compactions;",
             "DROP TABLE compactions;",
             "ALTER TABLE compactions_v11 RENAME TO compactions;",
             "CREATE INDEX IF NOT EXISTS idx_compactions_session"
             " ON compactions(session_id, at_seq);",
         }},
    }};
    // The invariant the version arithmetic depends on, checked where the list
    // is built: kSchemaVersion is the TARGET of ApplyMigrations, so it must
    // name the LAST migration's version. A migration added without the bump
    // (or a bump without its migration) would otherwise set user_version past
    // statements that never ran -- re-applied or skipped forever, silently.
    // Custom lists injected through OpenOptions (the test seam) are exempt:
    // they exist to exercise partial histories against a chosen target.
    if (migrations.empty() || migrations.back().version != SessionDatabase::kSchemaVersion) {
        throw std::runtime_error(
            "DefaultMigrations does not end at kSchemaVersion (" +
            std::to_string(SessionDatabase::kSchemaVersion) + ") -- a migration was added "
            "without the schema-version bump, or the bump landed without its migration");
    }
    return migrations;
}

// The terminating code of a row loop, checked. `while (step() == SQLITE_ROW)`
// exits on SQLITE_DONE and equally on SQLITE_BUSY, SQLITE_IOERR and
// SQLITE_CORRUPT -- so a mid-scan failure returned a SHORT result that every
// caller read as a complete one. On the resume path that is a transcript with a
// hole in it, which the loop then continues appending to at MAX(seq)+1; on the
// scrollback path it is a truncated page rendered as the top of the
// conversation. The JSON decode below these loops is strict about a row it
// cannot parse, which made the strict half a guard over the lax half's output.
void CheckRowLoopEnd(sqlite3* db, int rc, const std::string& context) {
    if (rc != SQLITE_DONE) ThrowSqliteError(db, context);
}

// Reads user_version, refuses a database ahead of this binary, and applies every
// pending migration inside one transaction. A failure anywhere rolls back to the
// version the database was opened at -- never a half-applied schema.
void ApplyMigrations(sqlite3* db, int target_version, const std::vector<Migration>& migrations) {
    // The list is checked before any I/O: it is a compile-time-ish invariant of
    // the caller, not a property of this database.
    int last_seen = 0;
    for (const auto& migration : migrations) {
        if (migration.version <= last_seen) {
            throw std::runtime_error("migration list is not strictly increasing in version");
        }
        last_seen = migration.version;
    }

    // IMMEDIATE FIRST, then read the version INSIDE the transaction. Reading it
    // outside was a time-of-check/time-of-use race: two processes starting
    // together could both see 0, both try to apply v1, and the loser would fail
    // to start with "table already exists". Narrow today, but the client
    // auto-spawn that is deferred out of Milestone A is exactly what makes two
    // concurrent starts routine.
    //
    // IMMEDIATE also takes the write lock up front, so the busy timeout applies
    // to the whole migration rather than only its first statement -- which is
    // what gives the loser of that race something to wait on instead of an error.
    ExecChecked(db, "BEGIN IMMEDIATE;", "failed to begin migration");
    try {
        int current = 0;
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &stmt, nullptr) != SQLITE_OK) {
            ThrowSqliteError(db, "failed to read schema version");
        }
        // A step ERROR is not "version 0": silently treating it as 0 would
        // re-run every migration and die on the first CREATE TABLE with a
        // "table already exists" that names the wrong disease. PRAGMA
        // user_version yields exactly one row, so SQLITE_DONE is the only
        // non-row success.
        const int step_rc = sqlite3_step(stmt);
        if (step_rc == SQLITE_ROW) {
            current = sqlite3_column_int(stmt, 0);
        } else if (step_rc != SQLITE_DONE) {
            const std::string message = sqlite3_errmsg(db);
            sqlite3_finalize(stmt);
            throw std::runtime_error("failed to read schema version: " + message);
        }
        sqlite3_finalize(stmt);

        if (current > target_version) {
            throw std::runtime_error(
                "session database schema version " + std::to_string(current) +
                " is newer than this binary understands (" + std::to_string(target_version) +
                "); refusing to open -- do not downgrade over a newer database");
        }

        std::vector<const Migration*> pending;
        for (const auto& migration : migrations) {
            if (migration.version > current) pending.push_back(&migration);
        }
        if (pending.empty()) {
            ExecChecked(db, "COMMIT;", "failed to close the migration check");
            return;
        }

        for (const auto* migration : pending) {
            for (const auto& statement : migration->statements) {
                ExecChecked(db, statement.c_str(), "migration v" + std::to_string(migration->version));
            }
        }
        ExecChecked(db, ("PRAGMA user_version = " + std::to_string(target_version) + ";").c_str(),
                    "failed to record schema version");
        ExecChecked(db, "COMMIT;", "failed to commit migration");
    } catch (...) {
        store::BestEffortRollback(db);
        throw;
    }
}

// Reuses a prepared statement across calls. sqlite3_prepare is cheap next to
// the work most statements do, but AppendMessage runs four of them per message
// on the path every turn pays, so the hot ones live in SessionDatabase's cache.
// Every public method holds mu_, so the cache needs no lock of its own. Reset
// (never finalize) on scope exit: the statement object stays owned by the
// cache, which finalizes it when the database closes.
class CachedStmt {
public:
    CachedStmt(sqlite3* db, std::unordered_map<std::string, sqlite3_stmt*>& cache,
               const char* sql, const std::string& context) {
        const std::string key(sql);
        auto it = cache.find(key);
        if (it == cache.end()) {
            sqlite3_stmt* fresh = nullptr;
            if (sqlite3_prepare_v2(db, sql, -1, &fresh, nullptr) != SQLITE_OK) {
                ThrowSqliteError(db, context);
            }
            it = cache.emplace(key, fresh).first;
        } else {
            sqlite3_reset(it->second);
            sqlite3_clear_bindings(it->second);
        }
        stmt = it->second;
    }
    // Reset AND clear. Reset alone left the last call's bound pointers in the
    // cached statement until its next use -- harmless while every binding was
    // SQLITE_TRANSIENT (SQLite owned copies), but AppendMessage now binds
    // content_json as SQLITE_STATIC, and a dangling borrowed pointer parked in a
    // long-lived statement is not something to leave to "nothing steps it".
    ~CachedStmt() {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
    }
    CachedStmt(const CachedStmt&) = delete;
    CachedStmt& operator=(const CachedStmt&) = delete;

    sqlite3_stmt* stmt = nullptr;
};

// Shape shared by every "seq, content_json, token_count, created_at" query --
// collected raw under mu_, decoded (the expensive JSON parse) outside it.
struct RawRow {
    std::int64_t seq;
    std::string json;
    int token_count;
    std::int64_t created_at;
    MessageAttribution attribution;
};

// A nullable TEXT column as a string: NULL (unknown) reads back as "".
std::string ColumnTextOrEmpty(sqlite3_stmt* stmt, int column) {
    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, column));
    return text != nullptr ? std::string(text) : std::string();
}

// Binds a string as TEXT, or NULL when empty -- the column's "unknown".
void BindTextOrNull(sqlite3_stmt* stmt, int index, const std::string& value) {
    if (value.empty()) {
        sqlite3_bind_null(stmt, index);
    } else {
        sqlite3_bind_text(stmt, index, value.c_str(), -1, SQLITE_TRANSIENT);
    }
}

// The collect half: steps a prepared, already-bound statement of the
// "seq, content_json, token_count, created_at" shape to completion. Must be
// called with mu_ held; `context` names the caller's own error message so a
// step failure still reads as theirs.
std::vector<RawRow> CollectRows(sqlite3_stmt* stmt, sqlite3* db, const std::string& context) {
    std::vector<RawRow> rows;
    int rc = SQLITE_OK;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        RawRow row;
        row.seq = sqlite3_column_int64(stmt, 0);
        row.json = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        row.token_count = sqlite3_column_int(stmt, 2);
        row.created_at = sqlite3_column_int64(stmt, 3);
        row.attribution.agent_preset = ColumnTextOrEmpty(stmt, 4);
        row.attribution.model = ColumnTextOrEmpty(stmt, 5);
        rows.push_back(std::move(row));
    }
    CheckRowLoopEnd(db, rc, context);
    return rows;
}

// The decode half: strict, meant to run OUTSIDE mu_ -- the JSON parse is the
// expensive part of a large resume or page, and holding the one database
// mutex through it would stall every other session's appends and reads.
StoredMessage DecodeRow(RawRow&& row) {
    StoredMessage stored;
    stored.seq = row.seq;
    stored.message = JsonToMessage(nlohmann::json::parse(std::move(row.json)));
    stored.token_count = row.token_count;
    stored.created_at_unix_ms = row.created_at;
    stored.attribution = std::move(row.attribution);
    return stored;
}

std::vector<StoredMessage> DecodeRows(std::vector<RawRow> rows) {
    std::vector<StoredMessage> out;
    out.reserve(rows.size());
    for (auto& row : rows) out.push_back(DecodeRow(std::move(row)));
    return out;
}

// One sessions row -> SessionSummary. Column order is the contract shared by
// GetSession and kSessionsPageSql (0..11); the two used to carry their own
// copies of this mapping, and the list query's copy silently lagged the
// columns the detail query grew (fork source, model overrides).
SessionSummary ReadSummaryRow(sqlite3_stmt* stmt) {
    const auto text = [stmt](int column) {
        return reinterpret_cast<const char*>(sqlite3_column_text(stmt, column));
    };
    const auto optional_text = [stmt, &text](int column) -> std::optional<std::string> {
        if (sqlite3_column_type(stmt, column) != SQLITE_TEXT) return std::nullopt;
        return std::string(text(column));
    };
    SessionSummary summary;
    summary.session_id = text(0);
    if (auto parent = optional_text(1)) summary.parent_session_id = std::move(*parent);
    summary.name = text(2);
    summary.name_source = text(3);
    summary.project_root = text(4);
    summary.agent_preset = text(5);
    summary.created_at_unix_ms = sqlite3_column_int64(stmt, 6);
    summary.updated_at_unix_ms = sqlite3_column_int64(stmt, 7);
    summary.message_count = sqlite3_column_int(stmt, 8);
    if (auto forked = optional_text(9)) summary.forked_from_session_id = std::move(*forked);
    if (auto model = optional_text(10)) summary.model = std::move(*model);
    if (auto sub = optional_text(11)) summary.subagent_model = std::move(*sub);
    return summary;
}

}  // namespace

const std::vector<Migration>& DefaultMigrations() { return DefaultMigrationsImpl(); }

SessionDatabase::SessionDatabase(const std::filesystem::path& db_path, const OpenOptions& options)
    : path_(db_path) {
    // The directory FIRST: ExclusiveFileLock opens `db_path + ".lock"` below,
    // and open() with O_CREAT still fails ENOENT when a DIRECTORY component
    // is missing (O_CREAT only covers the leaf file) -- a user-configured path
    // with no parent used to die with that misleading error instead of just
    // working. Best-effort: a real failure falls through to the lock open's
    // own error below.
    std::error_code ec;
    if (path_.has_parent_path()) {
        std::filesystem::create_directories(path_.parent_path(), ec);
    }

    // The single-writer assumption this class is built on, made structural: a
    // second server (or CLI) on the same database is refused at open instead
    // of silently interleaving appends with divergent working sets. Held for
    // the process lifetime; the member order below releases it before the
    // SQLite connection closes.
    lock_ = std::make_unique<store::ExclusiveFileLock>(db_path);

    // This file holds verbatim transcripts and tool output, which routinely
    // carry secrets -- the same material atomic_write's credentials-store
    // convention creates at 0600 -- so it must never be group/other-readable.
    // Create it owner-only ourselves rather than letting sqlite3_open apply
    // the umask (0644&~umask); sqlite3_open then finds the existing inode. A
    // failure here falls through to sqlite3_open's own error handling below
    // rather than masking it. The -wal/-shm sidecars inherit this mode.
    // O_NOFOLLOW: a symlink planted at path_ before this runs must not cause an
    // 0600-owner-only create to land on some other file the caller does not
    // control -- the same reasoning behind ExclusiveFileLock's O_NOFOLLOW.
    if (const int fd = ::open(path_.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
        fd >= 0) {
        ::close(fd);
    }

    if (sqlite3_open(path_.c_str(), &db_) != SQLITE_OK) {
        const std::string message = db_ ? sqlite3_errmsg(db_) : "sqlite3_open failed";
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("failed to open session database " + path_.string() + ": " + message);
    }

    // WAL: concurrent read-while-write and a crash mid-write leaves the last
    // committed state intact. foreign_keys is OFF by default in SQLite and the
    // cascade deletes depend on it. synchronous=NORMAL is WAL's recommended
    // setting (durability under process crash, not power loss). busy_timeout
    // covers out-of-process readers rather than failing them instantly.
    // wal_autocheckpoint=0: the default (checkpoint every ~1000 WAL pages)
    // landed checkpoint I/O inside a committing append -- under mu_, on the
    // tool-result path -- instead of on the reaper's tick (see
    // MaybeRunPassiveCheckpoint).
    //
    // Everything from here can throw after sqlite3_open succeeded, and a
    // constructor that exits via exception never runs the destructor (the
    // object was never fully constructed) -- so without this try/catch the
    // sqlite handle (fd + memory) and the -wal/-shm sidecars would leak. Close
    // and rethrow.
    try {
        for (const char* pragma : {"PRAGMA journal_mode=WAL;", "PRAGMA foreign_keys=ON;",
                                   "PRAGMA synchronous=NORMAL;", "PRAGMA busy_timeout=5000;",
                                   "PRAGMA wal_autocheckpoint=0;"}) {
            ExecChecked(db_, pragma, std::string("failed to set ") + pragma);
        }

        const auto& migrations = options.migrations ? *options.migrations : DefaultMigrations();
        ApplyMigrations(db_, kSchemaVersion, migrations);

        // Orphan sweep: message or checkpoint rows whose session is gone. This
        // binary deletes messages BEFORE the session row (see DeleteSession), so a
        // crash mid-delete leaves a session with a partial transcript, not
        // orphans -- the sweep exists for databases written by paths that cannot
        // make that promise (one written while foreign_keys was off, or by a
        // crashed older binary). One indexed pass at open: NOT EXISTS probes
        // sessions through the (session_id, ...) index prefix per row rather than
        // scanning the sessions table per message.
        //
        // Best-effort by design: a failure (a corrupt page in the messages table,
        // a busy disk) is rolled back and swallowed, because the sweep is hygiene
        // rather than correctness -- refusing to open over it would make a
        // corrupt-but-servable database unstartable, and the corruption surfaces
        // at read time anyway.
        try {
            ExecChecked(db_, "BEGIN IMMEDIATE;", "failed to begin orphan sweep");
            ExecChecked(db_,
                        "DELETE FROM messages WHERE NOT EXISTS"
                        " (SELECT 1 FROM sessions s WHERE s.id = messages.session_id);",
                        "failed to sweep orphaned messages");
            ExecChecked(db_,
                        "DELETE FROM compactions WHERE NOT EXISTS"
                        " (SELECT 1 FROM sessions s WHERE s.id = compactions.session_id);",
                        "failed to sweep orphaned compactions");
            ExecChecked(db_, "COMMIT;", "failed to commit orphan sweep");
        } catch (...) {
            char* err = nullptr;
            if (sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, &err) != SQLITE_OK && err) {
                sqlite3_free(err);
            }
        }
    } catch (...) {
        sqlite3_close(db_);
        db_ = nullptr;
        throw;
    }
}

SessionDatabase::~SessionDatabase() {
    if (db_) {
        // Cached statements must die before their connection.
        for (auto& [sql, stmt] : prepared_) {
            sqlite3_finalize(stmt);
        }
        prepared_.clear();
        // A clean close of the last connection checkpoints the WAL and removes
        // the -wal/-shm side files, leaving one self-contained database file.
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

std::int64_t SessionDatabase::NextTimestamp() {
    const std::int64_t now = NowMillis();
    last_ts_ = (now > last_ts_) ? now : last_ts_ + 1;
    return last_ts_;
}

SessionSummary SessionDatabase::CreateSession(std::string name,
                                              std::optional<std::string> parent_session_id,
                                              std::string project_root, std::string agent_preset,
                                              std::optional<std::string> model) {
    std::lock_guard lock(mu_);
    const std::int64_t now = NextTimestamp();

    if (parent_session_id) {
        // A clear error here instead of an FK violation surfacing from the insert.
        Stmt check(db_, "SELECT 1 FROM sessions WHERE id = ?;", "failed to check parent session");
        sqlite3_bind_text(check.stmt, 1, parent_session_id->c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(check.stmt) != SQLITE_ROW) {
            throw std::runtime_error("parent session not found: " + *parent_session_id);
        }
    }

    const std::string id = GenerateUlid();
    // An explicitly named session is recorded as such (see IsExplicitlyNamed);
    // the auto-naming rule and the rename seam both honor it later.
    const char* name_source = name.empty() ? "" : "explicit";
    Stmt insert(
        db_,
        "INSERT INTO sessions (id, parent_session_id, name, name_source, project_root,"
        " agent_preset, created_at, updated_at, model) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);",
        "failed to create session");
    sqlite3_bind_text(insert.stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    if (parent_session_id) {
        sqlite3_bind_text(insert.stmt, 2, parent_session_id->c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(insert.stmt, 2);
    }
    sqlite3_bind_text(insert.stmt, 3, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert.stmt, 4, name_source, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert.stmt, 5, project_root.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert.stmt, 6, agent_preset.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert.stmt, 7, now);
    sqlite3_bind_int64(insert.stmt, 8, now);
    if (model) {
        sqlite3_bind_text(insert.stmt, 9, model->c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(insert.stmt, 9);
    }
    if (sqlite3_step(insert.stmt) != SQLITE_DONE) {
        ThrowSqliteError(db_, "failed to create session");
    }

    return SessionSummary{std::move(id),          std::move(parent_session_id),
                          std::move(name),        name_source,
                          std::move(project_root), std::move(agent_preset),
                          now,                    now,
                          0,                      {},
                          std::move(model),       std::nullopt};
}

std::int64_t SessionDatabase::AppendMessage(const std::string& session_id,
                                            const llm::Message& message, int token_count,
                                            const MessageAttribution& attribution) {
    const AppendEntry entry{&message, token_count, attribution};
    return AppendMessages(session_id, std::span<const AppendEntry>(&entry, 1));
}

std::int64_t SessionDatabase::AppendMessages(const std::string& session_id,
                                             std::span<const AppendEntry> entries) {
    if (entries.empty()) throw std::logic_error("AppendMessages: no entries");
    // Encode OUTSIDE the mutex. mu_ is the one lock every session's appends and
    // reads serialize on, and the JSON encode -- the DOM plus the dump -- is the
    // expensive half of an append. Holding the lock through it stalled every
    // other session behind one session's tool result for the whole encode.
    // The transaction itself (and NextTimestamp, which mutates last_ts_) still
    // runs under the lock; only pure computation over the caller's message is
    // hoisted.
    //
    // REPLACE, not the default strict handler. dump() throws type_error.316 on
    // invalid UTF-8, and nothing between a child process's raw stdout and this
    // line validates encoding -- one `cat` of a binary file, or a tool printing
    // Latin-1, threw here. That throw arrived BEFORE the transaction, so the
    // message was persisted nowhere and AgentLoop::AppendAndPersist never
    // reached its history_.push_back either: the assistant's tool_use was
    // already in history, its results were not, and every later turn 400'd on
    // the orphaned call. Substituting U+FFFD keeps a byte of noise from
    // destroying a conversation; the transcript is a record of what was said,
    // not a binary-faithful channel.
    std::vector<std::string> content_jsons;
    content_jsons.reserve(entries.size());
    for (const AppendEntry& entry : entries) {
        content_jsons.push_back(MessageToJson(*entry.message)
                                    .dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
    }

    std::lock_guard lock(mu_);

    std::int64_t assigned_seq = 0;
    ExecChecked(db_, "BEGIN IMMEDIATE;", "failed to begin append");
    try {
        // THE FORK, and it must be the transaction's first statement. The first
        // append after a rewind permanently discards the marked tail: doing it
        // here makes the branch discard atomic with the append that replaces
        // it (a crash before this transaction keeps redo alive; a crash after
        // it leaves the fork standing), and it guarantees the MAX(seq)
        // subquery below reads the ACTIVE transcript -- the new append
        // continues the sequence the user sees rather than landing above rows
        // they rewound away.
        CachedStmt reclaim(db_, prepared_,
                           "DELETE FROM messages WHERE session_id = ? AND discarded = 1;",
                           "failed to reclaim discarded messages");
        sqlite3_bind_text(reclaim.stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(reclaim.stmt) != SQLITE_DONE) {
            ThrowSqliteError(db_, "failed to reclaim discarded messages");
        }
        // Compactions that ran on the discarded branch go with it: their
        // at_seq positions are about to be reused by the new branch's seqs.
        CachedStmt reclaim_compactions(
            db_, prepared_,
            "DELETE FROM compactions WHERE session_id = ?1 AND at_seq >"
            " COALESCE((SELECT MAX(seq) FROM messages WHERE session_id = ?1), -1);",
            "failed to reclaim discarded compactions");
        sqlite3_bind_text(reclaim_compactions.stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(reclaim_compactions.stmt) != SQLITE_DONE) {
            ThrowSqliteError(db_, "failed to reclaim discarded compactions");
        }

        // ALL entries land in this one transaction: a tool round's assistant
        // message and its results commit together or not at all, so no reader
        // or resume ever sees a tool_use without its results.
        for (std::size_t i = 0; i < entries.size(); ++i) {
        const AppendEntry& entry = entries[i];
        const std::string& content_json = content_jsons[i];
        const char* role = entry.message->role == llm::Role::User ? "user" : "assistant";
        const MessageAttribution& attribution = entry.attribution;
        const int token_count = entry.token_count;
        const std::int64_t now = NextTimestamp();
        // Validate the session and touch last-activity first: appending to a
        // session that does not exist is a wiring bug and must be loud, not a
        // silently dropped row. The message_count bump rides the same
        // statement -- and the same transaction, so the maintained count the
        // picker reads is exact even across a crash mid-append.
        CachedStmt touch(db_, prepared_,
                         "UPDATE sessions SET updated_at = ?, message_count = message_count + 1"
                         " WHERE id = ?;",
                         "failed to touch session");
        sqlite3_bind_int64(touch.stmt, 1, now);
        sqlite3_bind_text(touch.stmt, 2, session_id.c_str(), -1, SQLITE_TRANSIENT);
        // The two conditions are SEPARATE because they are different problems.
        // Folding them reported a full disk, a locked database or an I/O error
        // as "unknown session" -- and the comment above says this message is
        // meant to be loud about a WIRING BUG, so it sent the next reader
        // hunting one that was not there.
        if (const int rc = sqlite3_step(touch.stmt); rc != SQLITE_DONE) {
            ThrowSqliteError(db_, "failed to touch session " + session_id);
        }
        if (sqlite3_changes(db_) == 0) {
            throw std::runtime_error("cannot append to unknown session: " + session_id);
        }

        // seq is the next per-session value over the ACTIVE transcript. The
        // discarded=0 filter is defense in depth -- the reclaim above should
        // have removed every marked row -- because a collision here would
        // otherwise be a silent seq gap the contiguous-suffix invariant
        // cannot describe. The UNIQUE(session_id, seq) constraint is the
        // backstop if this arithmetic is ever wrong either way.
        CachedStmt insert(
            db_, prepared_,
            "INSERT INTO messages (session_id, seq, role, content_json, token_count, created_at,"
            " agent_preset, model)"
            " VALUES (?, COALESCE((SELECT MAX(seq) FROM messages"
            " WHERE session_id = ? AND discarded = 0), -1) + 1,"
            " ?, ?, ?, ?, ?, ?);",
            "failed to append message");
        sqlite3_bind_text(insert.stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert.stmt, 2, session_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert.stmt, 3, role, -1, SQLITE_STATIC);
        // STATIC with an explicit length, not TRANSIENT with a strlen. content_json
    // is the largest thing on this path -- a tool result can be hundreds of KB --
    // and TRANSIENT tells SQLite to malloc and memcpy the whole thing, a third
    // full copy after the JSON DOM and the dump(). It outlives the step() below,
    // and ~CachedStmt clears bindings, so nothing retains the pointer.
    sqlite3_bind_text(insert.stmt, 4, content_json.data(),
                      static_cast<int>(content_json.size()), SQLITE_STATIC);
        sqlite3_bind_int(insert.stmt, 5, token_count);
        sqlite3_bind_int64(insert.stmt, 6, now);
        BindTextOrNull(insert.stmt, 7, attribution.agent_preset);
        BindTextOrNull(insert.stmt, 8, attribution.model);
        if (sqlite3_step(insert.stmt) != SQLITE_DONE) {
            ThrowSqliteError(db_, "failed to append message");
        }

        // Still inside the transaction, so this reads the row just written and
        // cannot see another writer's -- and there is only one writer anyway.
        CachedStmt read_seq(db_, prepared_, "SELECT seq FROM messages WHERE id = ?;",
                            "failed to read back seq");
        sqlite3_bind_int64(read_seq.stmt, 1, sqlite3_last_insert_rowid(db_));
        if (sqlite3_step(read_seq.stmt) != SQLITE_ROW) {
            ThrowSqliteError(db_, "failed to read back seq");
        }
        assigned_seq = sqlite3_column_int64(read_seq.stmt, 0);
        }

        ExecChecked(db_, "COMMIT;", "failed to commit append");
    } catch (...) {
        char* err = nullptr;
        if (sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, &err) != SQLITE_OK && err) {
            sqlite3_free(err);
        }
        throw;
    }
    // The reaper's threshold, not the transaction's: a committed append counts
    // even though the counter is checked without mu_ (see the member comment).
    appends_since_checkpoint_.fetch_add(static_cast<int>(entries.size()),
                                        std::memory_order_release);
    return assigned_seq;
}

SessionDatabase::CheckpointOutcome SessionDatabase::MaybeRunPassiveCheckpoint() {
    if (appends_since_checkpoint_.load(std::memory_order_acquire) < kCheckpointAppends) {
        return CheckpointOutcome::kNotDue;
    }
    // Claim the accumulated appends BEFORE deciding the checkpoint is due, so
    // appends landing during the checkpoint are counted toward the next one.
    // (A failed attempt below therefore also waits for kCheckpointAppends more
    // appends before retrying -- that throttle is deliberate: retrying every
    // reaper tick would hold mu_ behind a busy-timeout-bounded RESTART each
    // time. The failure is surfaced through checkpoint_failures() instead.)
    appends_since_checkpoint_.store(0, std::memory_order_relaxed);
    std::lock_guard lock(mu_);
    // RESTART, not PASSIVE -- a measured correction to the plan's wording.
    // PASSIVE backfills frames into the main database but NEVER resets the
    // log, so with autocheckpoint disabled the WAL would grow without bound
    // (verified: 1043 frames backfilled, a fresh reader still saw all 1043).
    // RESTART backfills the same frames and then ensures the next writer
    // restarts from the head of the log. The blocking RESTART accepts is only
    // against out-of-process readers -- in-process there is exactly one
    // connection and the reaper already holds mu_ -- and it is bounded by
    // busy_timeout (5 s), rare (the startup sanity check, a future CLI), and
    // amortized: once per kCheckpointAppends appends instead of inside every
    // committing append. A busy result loses nothing; the frames remain and
    // a later attempt picks them up. Failures are deliberately not thrown:
    // the reaper is the caller, and a failed best-effort optimization must not
    // take the tick down -- but they are counted and returned, so the wiring
    // can make a persistently pinned WAL visible instead of swallowing it.
    const int rc = sqlite3_wal_checkpoint_v2(db_, nullptr, SQLITE_CHECKPOINT_RESTART, nullptr, nullptr);
    if (rc == SQLITE_OK) {
        checkpoint_failures_.store(0, std::memory_order_relaxed);
        return CheckpointOutcome::kCompleted;
    }
    checkpoint_failures_.fetch_add(1, std::memory_order_relaxed);
    return CheckpointOutcome::kFailed;
}

int SessionDatabase::wal_autocheckpoint() const {
    std::lock_guard lock(mu_);
    // There is no C getter (sqlite3_wal_autocheckpoint is setter-only), so the
    // pragma reports the connection's own current value.
    Stmt query(db_, "PRAGMA wal_autocheckpoint;", "failed to read wal_autocheckpoint");
    if (sqlite3_step(query.stmt) != SQLITE_ROW) {
        ThrowSqliteError(db_, "failed to read wal_autocheckpoint");
    }
    return sqlite3_column_int(query.stmt, 0);
}

bool SessionDatabase::SetDefaultName(const std::string& session_id, const std::string& name) {
    std::lock_guard lock(mu_);
    // Only when the session still has no name: an explicit creation name or a
    // name an observer plugin set later is never clobbered. The source is
    // recorded with it (migration v6), so the rename seam can tell a
    // generated title from a default apart from a user's choice.
    CachedStmt rename(db_, prepared_,
                      "UPDATE sessions SET name = ?, name_source = 'default'"
                      " WHERE id = ? AND name = '';",
                      "failed to set default session name");
    sqlite3_bind_text(rename.stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(rename.stmt, 2, session_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(rename.stmt) != SQLITE_DONE) {
        ThrowSqliteError(db_, "failed to set default session name");
    }
    return sqlite3_changes(db_) == 1;
}

bool SessionDatabase::UpdateSessionName(const std::string& session_id, const std::string& name,
                                        std::string_view name_source) {
    std::lock_guard lock(mu_);
    Stmt update(db_, "UPDATE sessions SET name = ?, name_source = ?, updated_at = ?"
                     " WHERE id = ?;",
               "failed to rename session");
    sqlite3_bind_text(update.stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(update.stmt, 2, name_source.data(),
                      static_cast<int>(name_source.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(update.stmt, 3, NextTimestamp());
    sqlite3_bind_text(update.stmt, 4, session_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(update.stmt) != SQLITE_DONE) {
        ThrowSqliteError(db_, "failed to rename session");
    }
    return sqlite3_changes(db_) == 1;
}

bool SessionDatabase::UpdateSessionNameUnlessExplicit(const std::string& session_id,
                                                      const std::string& name,
                                                      std::string_view name_source) {
    std::lock_guard lock(mu_);
    Stmt update(db_,
               "UPDATE sessions SET name = ?, name_source = ?, updated_at = ?"
               " WHERE id = ? AND name_source != 'explicit';",
               "failed to rename session");
    sqlite3_bind_text(update.stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(update.stmt, 2, name_source.data(),
                      static_cast<int>(name_source.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(update.stmt, 3, NextTimestamp());
    sqlite3_bind_text(update.stmt, 4, session_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(update.stmt) != SQLITE_DONE) {
        ThrowSqliteError(db_, "failed to rename session");
    }
    return sqlite3_changes(db_) == 1;
}

bool SessionDatabase::UpdateSessionModel(const std::string& session_id,
                                         const std::optional<std::string>& model) {
    std::lock_guard lock(mu_);
    // No updated_at touch: a model switch is not transcript activity, and
    // updated_at is the picker's sort key -- /model must not reorder the list.
    Stmt update(db_, "UPDATE sessions SET model = ? WHERE id = ?;",
                "failed to update session model");
    if (model) {
        sqlite3_bind_text(update.stmt, 1, model->c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(update.stmt, 1);
    }
    sqlite3_bind_text(update.stmt, 2, session_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(update.stmt) != SQLITE_DONE) {
        ThrowSqliteError(db_, "failed to update session model");
    }
    return sqlite3_changes(db_) == 1;
}

bool SessionDatabase::UpdateSessionSubagentModel(const std::string& session_id,
                                                 const std::optional<std::string>& model) {
    std::lock_guard lock(mu_);
    // No updated_at touch, same reasoning as UpdateSessionModel.
    Stmt update(db_, "UPDATE sessions SET subagent_model = ? WHERE id = ?;",
                "failed to update session subagent model");
    if (model) {
        sqlite3_bind_text(update.stmt, 1, model->c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(update.stmt, 1);
    }
    sqlite3_bind_text(update.stmt, 2, session_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(update.stmt) != SQLITE_DONE) {
        ThrowSqliteError(db_, "failed to update session subagent model");
    }
    return sqlite3_changes(db_) == 1;
}

bool SessionDatabase::IsExplicitlyNamed(const std::string& session_id) const {
    std::lock_guard lock(mu_);
    CachedStmt query(db_, prepared_,
                     "SELECT 1 FROM sessions WHERE id = ? AND name_source = 'explicit';",
                     "failed to check session name source");
    sqlite3_bind_text(query.stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(query.stmt);
    if (rc == SQLITE_ROW) return true;
    // A lock or I/O failure is not "not explicitly named" -- same ruling as
    // SessionExists.
    CheckRowLoopEnd(db_, rc, "failed to check session name source");
    return false;
}

bool SessionDatabase::DeleteSession(const std::string& session_id) {
    std::lock_guard lock(mu_);

    // The doomed set: the session and every descendant (the self-FK cascade
    // used to reach children implicitly; the explicit set must, too).
    std::vector<std::string> doomed;
    {
        Stmt collect(db_,
                     "WITH RECURSIVE doomed(id) AS ("
                     "  SELECT id FROM sessions WHERE id = ?1"
                     "  UNION ALL"
                     "  SELECT s.id FROM sessions s JOIN doomed d ON s.parent_session_id = d.id"
                     ") SELECT id FROM doomed;",
                     "failed to collect doomed sessions");
        sqlite3_bind_text(collect.stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
        int rc = SQLITE_OK;
        while ((rc = sqlite3_step(collect.stmt)) == SQLITE_ROW) {
            const auto* id = reinterpret_cast<const char*>(sqlite3_column_text(collect.stmt, 0));
            doomed.emplace_back(id);
        }
        CheckRowLoopEnd(db_, rc, "failed to collect doomed sessions");
    }
    if (doomed.empty()) return false;

    // Chunked deletes, one sub-transaction per chunk -- NOT one transaction
    // around the whole delete. The old single-statement cascade held mu_ while
    // it walked and removed the ENTIRE transcript: for a 100k-message session
    // that is every other session's appends and reads stalled behind one
    // DELETE, minutes on slow storage. Chunks cost the same total work but
    // interleave, so the stall per other-session operation drops from
    // whole-transcript to one chunk. The tradeoff, deliberate: a crash
    // mid-delete leaves the session row (and a PARTIAL transcript) behind
    // rather than all-or-nothing -- the operator deletes again, and the
    // startup orphan sweep cleans what a crash can orphan. message_count is
    // decremented inside each chunk's transaction, so even the partial state
    // keeps the count consistent with the rows that survive.
    constexpr int kChunk = 500;
    {
        Stmt chunk(db_, "DELETE FROM messages WHERE id IN"
                        " (SELECT id FROM messages WHERE session_id = ?1 LIMIT ?2);",
                   "failed to delete message chunk");
        Stmt count(db_, "UPDATE sessions SET message_count = MAX(0, message_count - ?1)"
                        " WHERE id = ?2;",
                  "failed to update session message_count for delete chunk");
        for (const auto& id : doomed) {
            std::size_t removed = 0;
            do {
                ExecChecked(db_, "BEGIN IMMEDIATE;", "failed to begin delete chunk");
                sqlite3_reset(chunk.stmt);
                sqlite3_clear_bindings(chunk.stmt);
                sqlite3_bind_text(chunk.stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(chunk.stmt, 2, kChunk);
                if (sqlite3_step(chunk.stmt) != SQLITE_DONE) {
                    // Capture the original error BEFORE rolling back: a failed
                    // rollback overwrites sqlite3_errmsg and would mask the
                    // real cause behind a rollback message.
                    const std::string original = sqlite3_errmsg(db_);
                    BestEffortRollback(db_);
                    throw std::runtime_error("failed to delete message chunk: " + original);
                }
                removed = static_cast<std::size_t>(sqlite3_changes(db_));
                if (removed > 0) {
                    sqlite3_reset(count.stmt);
                    sqlite3_clear_bindings(count.stmt);
                    sqlite3_bind_int64(count.stmt, 1, static_cast<sqlite3_int64>(removed));
                    sqlite3_bind_text(count.stmt, 2, id.c_str(), -1, SQLITE_TRANSIENT);
                    if (sqlite3_step(count.stmt) != SQLITE_DONE) {
                        const std::string original = sqlite3_errmsg(db_);
                        BestEffortRollback(db_);
                        throw std::runtime_error(
                            "failed to update session message_count for delete chunk: " +
                            original);
                    }
                }
                ExecChecked(db_, "COMMIT;", "failed to commit delete chunk");
            } while (removed == static_cast<std::size_t>(kChunk));
        }
    }

    // The rows themselves: compaction checkpoints ride the FK cascade, and the
    // (now message-less) session rows go in one statement -- a full-transcript
    // cascade no longer happens inside it, because there is nothing left to
    // cascade but the rows.
    ExecChecked(db_, "BEGIN IMMEDIATE;", "failed to begin session delete");
    try {
        for (const auto& id : doomed) {
            Stmt del(db_, "DELETE FROM sessions WHERE id = ?;", "failed to delete session");
            sqlite3_bind_text(del.stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(del.stmt) != SQLITE_DONE) {
                ThrowSqliteError(db_, "failed to delete session");
            }
        }
        ExecChecked(db_, "COMMIT;", "failed to commit session delete");
    } catch (...) {
        char* err = nullptr;
        if (sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, &err) != SQLITE_OK && err) {
            sqlite3_free(err);
        }
        throw;
    }
    return true;
}

std::optional<SessionSummary> SessionDatabase::GetSession(const std::string& session_id) const {
    std::lock_guard lock(mu_);
    // message_count is a maintained column (bumped in the append transaction,
    // backfilled by migration v2), not a correlated COUNT(*) -- the subquery
    // made every call pay O(messages-of-session) to fill a display field.
    CachedStmt query(db_, prepared_,
                     "SELECT s.id, s.parent_session_id, s.name, s.name_source, s.project_root,"
                     " s.agent_preset, s.created_at, s.updated_at, s.message_count,"
                     " s.forked_from_session_id, s.model, s.subagent_model"
                     " FROM sessions s WHERE s.id = ?;",
                     "failed to load session");
    sqlite3_bind_text(query.stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
    // The SessionExists ruling, swept here: a step failure is a STORE FAULT,
    // not a missing row. Returning nullopt for SQLITE_BUSY/SQLITE_IOERR/SQLITE_CORRUPT
    // made the resume handler answer "no such session" -- the one answer the
    // client cannot recover from -- for a session that was very much still on
    // disk, and the caller's unchecked dereference of the re-read below was UB
    // in the RPC handler. id is the PRIMARY KEY, so the first step decides:
    // ROW means present, DONE means genuinely absent, anything else throws.
    if (const int rc = sqlite3_step(query.stmt); rc != SQLITE_ROW) {
        CheckRowLoopEnd(db_, rc, "failed to load session");
        return std::nullopt;  // SQLITE_DONE: the row is not there
    }

    return ReadSummaryRow(query.stmt);
}

bool SessionDatabase::SessionExists(const std::string& session_id) const {
    std::lock_guard lock(mu_);
    // Deliberately NOT GetSession(): that carries the session's message_count,
    // which a presence check throws away. Paging a long transcript called this
    // once per page.
    CachedStmt query(db_, prepared_, "SELECT 1 FROM sessions WHERE id = ?;",
                     "failed to check session");
    sqlite3_bind_text(query.stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(query.stmt);
    if (rc == SQLITE_ROW) return true;
    // A LOCKED DATABASE IS NOT A MISSING SESSION. Returning false for
    // SQLITE_BUSY/SQLITE_IOERR exactly as for an absent row made the RPC layer
    // answer "no such session" -- so a transient lock told the client its
    // session had been deleted, which is the one answer it cannot recover from.
    CheckRowLoopEnd(db_, rc, "failed to check session");
    return false;
}

std::vector<SessionSummary> SessionDatabase::ListSessions(const ListSessionsFilter& filter) const {
    std::lock_guard lock(mu_);
    // kSessionsPageSql (in the header) carries the full story of the cursor:
    // a PAIR, because the sort key is, and a `<= COALESCE` ceiling the planner
    // constrains on rather than a `? IS NULL OR` prefix it plans as a scan.
    // message_count is the maintained column (see GetSession), not a correlated
    // COUNT(*) that would make the page cost O(total messages).
    CachedStmt query(db_, prepared_, kSessionsPageSql.data(), "failed to list sessions");
    if (filter.project_root) {
        sqlite3_bind_text(query.stmt, 1, filter.project_root->c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(query.stmt, 1);
    }
    sqlite3_bind_int(query.stmt, 2, filter.include_subagents ? 1 : 0);
    if (filter.before_updated_at) {
        sqlite3_bind_int64(query.stmt, 3, *filter.before_updated_at);
    } else {
        sqlite3_bind_null(query.stmt, 3);
    }
    if (filter.before_id) {
        sqlite3_bind_text(query.stmt, 4, filter.before_id->c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(query.stmt, 4);
    }
    // -1 is SQLite's "no limit", which keeps the unbounded case expressible for
    // a caller that genuinely wants every row (the sanity check, a future CLI).
    sqlite3_bind_int(query.stmt, 5, filter.limit > 0 ? filter.limit : -1);

    std::vector<SessionSummary> out;
    int rc = SQLITE_OK;
    while ((rc = sqlite3_step(query.stmt)) == SQLITE_ROW) {
        out.push_back(ReadSummaryRow(query.stmt));
    }
    CheckRowLoopEnd(db_, rc, "failed to list sessions");
    return out;
}

MessagePage SessionDatabase::GetMessagesPage(const std::string& session_id,
                                             std::optional<std::int64_t> before_seq,
                                             int limit) const {
    if (limit <= 0) {
        throw std::runtime_error("GetMessagesPage: limit must be > 0");
    }
    // Keyset pagination: the predicate is an index seek, so a page of a 1M-token
    // history costs the same as a page of a short one. Fetch one probe row past
    // the page to compute has_more without a second COUNT query.
    //
    // The cursor sentinel is INT64_MAX, not a `?2 IS NULL` branch. The branch
    // looks cleaner but defeats the planner: with `OR` binding a NULL, SQLite
    // plans this as equality on session_id and a plain index walk from
    // MAX(seq) -- every page request re-walked the whole range above the
    // cursor, so paging an n-message transcript was O(n) per page and O(n^2)
    // total. COALESCE folds the "no cursor" case into the same comparison, the
    // planner constrains on seq, and any real cursor is an exact seek. The
    // sentinel cannot collide with a row: seq values here are row indices.
    std::vector<RawRow> rows;
    bool has_more = false;
    {
        std::lock_guard lock(mu_);
        CachedStmt query(db_, prepared_, kMessagesPageSql.data(), "failed to page messages");
        sqlite3_bind_text(query.stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
        if (before_seq) {
            sqlite3_bind_int64(query.stmt, 2, *before_seq);
        } else {
            sqlite3_bind_null(query.stmt, 2);
        }
        // int64, not `limit + 1` as int: a limit at INT_MAX would overflow the
        // increment and bind a negative LIMIT, which SQLite reads as "no limit" --
        // a full-transcript materialization instead of a page.
        sqlite3_bind_int64(query.stmt, 3, static_cast<std::int64_t>(limit) + 1);

        // Collect raw rows under the lock; decode after. This is the hottest
        // read path (every scrollback page, every HistoryWindow read), and the
        // JSON parse under mu_ stalled every other session's appends and reads
        // for the whole page -- the LoadMessages fix, swept here rather than
        // rediscovered per call site.
        int rc = SQLITE_OK;
        while ((rc = sqlite3_step(query.stmt)) == SQLITE_ROW) {
            if (static_cast<int>(rows.size()) == limit) {
                has_more = true;
                // Deliberately leaves rc == SQLITE_ROW: this loop ends early ON
                // PURPOSE when the probe row appears, which is not an error and
                // must not be checked as one.
                break;
            }
            RawRow row;
            row.seq = sqlite3_column_int64(query.stmt, 0);
            row.json = reinterpret_cast<const char*>(sqlite3_column_text(query.stmt, 1));
            row.token_count = sqlite3_column_int(query.stmt, 2);
            row.created_at = sqlite3_column_int64(query.stmt, 3);
            row.attribution.agent_preset = ColumnTextOrEmpty(query.stmt, 4);
            row.attribution.model = ColumnTextOrEmpty(query.stmt, 5);
            rows.push_back(std::move(row));
        }
        // Reached only when the scan ran out of rows, so anything but DONE is a
        // real failure. Returning short here reported has_more = false, which
        // renders a truncated page as the top of the transcript.
        if (!has_more) {
            CheckRowLoopEnd(db_, rc, "failed to read message page");
        }
    }

    // Strict decode OUTSIDE the lock: a row that cannot be parsed is surfaced
    // here, at read time, rather than smuggled to the client as garbage. The
    // decoded message is trusted wholesale; the role column exists for
    // SQL-level filtering, not as a second source of truth.
    MessagePage page;
    page.has_more = has_more;
    page.messages = DecodeRows(std::move(rows));
    return page;
}

MessagePage SessionDatabase::GetMessagesPageAfter(const std::string& session_id,
                                                  std::int64_t after_seq, int limit) const {
    if (limit <= 0) {
        throw std::runtime_error("GetMessagesPageAfter: limit must be > 0");
    }
    // Same planner discipline as GetMessagesPage (its comment): a bound
    // cursor is an exact index seek, the probe row computes has_more without
    // a COUNT, and raw rows are collected under the lock and decoded after
    // it is released.
    std::vector<RawRow> rows;
    bool has_more = false;
    {
        std::lock_guard lock(mu_);
        CachedStmt query(db_, prepared_, kMessagesPageAfterSql.data(), "failed to page messages after seq");
        sqlite3_bind_text(query.stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(query.stmt, 2, after_seq);
        sqlite3_bind_int64(query.stmt, 3, static_cast<std::int64_t>(limit) + 1);
        int rc = SQLITE_OK;
        while ((rc = sqlite3_step(query.stmt)) == SQLITE_ROW) {
            if (static_cast<int>(rows.size()) == limit) {
                has_more = true;   // more NEWER rows remain (see the SQL comment)
                break;
            }
            RawRow row;
            row.seq = sqlite3_column_int64(query.stmt, 0);
            row.json = reinterpret_cast<const char*>(sqlite3_column_text(query.stmt, 1));
            row.token_count = sqlite3_column_int(query.stmt, 2);
            row.created_at = sqlite3_column_int64(query.stmt, 3);
            row.attribution.agent_preset = ColumnTextOrEmpty(query.stmt, 4);
            row.attribution.model = ColumnTextOrEmpty(query.stmt, 5);
            rows.push_back(std::move(row));
        }
        if (!has_more) {
            CheckRowLoopEnd(db_, rc, "failed to read message page after seq");
        }
    }
    MessagePage page;
    page.has_more = has_more;
    page.messages = DecodeRows(std::move(rows));   // ascending by seq
    return page;
}

std::vector<StoredMessage> SessionDatabase::LoadMessages(const std::string& session_id) const {
    // Ascending, no limit on the RESULT: the caller (resume) needs the whole
    // working history in order. Same strict decode as the paging path -- an
    // undecodable row is corruption or a newer binary's work, and must
    // surface as an error rather than as a silently truncated history that
    // would then be re-persisted.
    //
    // CHUNKED, not one unbounded SELECT: mu_ is the one lock every session's
    // appends and reads serialize on. A never-compacted session resuming at
    // this project's stated 512k-1M-token scale is exactly the load whose
    // single-query form held that lock -- and stalled every OTHER session's
    // in-flight turn -- for the time it took to walk and collect the entire
    // transcript. Releasing the lock between chunks lets a long resume
    // interleave with other sessions' appends instead of blocking them for
    // its whole duration; each chunk still collects its rows under the lock
    // (CollectRows is a caller-holds-mu_ contract), and only the JSON decode
    // -- the expensive half -- runs outside it, exactly as the unchunked
    // version already did.
    constexpr std::int64_t kChunkRows = 500;
    std::vector<StoredMessage> out;
    std::int64_t after_seq = -1;  // exclusive cursor; -1 selects from the start
    while (true) {
        std::vector<RawRow> rows;
        {
            std::lock_guard lock(mu_);
            CachedStmt query(db_, prepared_,
                             "SELECT seq, content_json, token_count, created_at, agent_preset,"
                             " model FROM messages"
                             " WHERE session_id = ?1 AND discarded = 0 AND seq > ?2"
                             " ORDER BY seq ASC LIMIT ?3;",
                             "failed to load messages");
            sqlite3_bind_text(query.stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(query.stmt, 2, after_seq);
            sqlite3_bind_int64(query.stmt, 3, kChunkRows);
            // THE RESUME PATH, and the most damaging place to return short: a
            // partial read becomes the loop's working history while
            // SqliteHistoryStore keeps appending at MAX(seq) + 1, so the
            // session runs on a transcript with a hole in it and nothing
            // ever says so.
            rows = CollectRows(query.stmt, db_, "failed to load messages");
        }
        if (rows.empty()) break;
        after_seq = rows.back().seq;
        const bool full_chunk = static_cast<std::int64_t>(rows.size()) == kChunkRows;

        // Decode OUTSIDE the lock. The JSON parse is the expensive part of a
        // large resume, and holding the one database mutex through it would
        // stall every other session's appends and reads for the whole load.
        auto decoded = DecodeRows(std::move(rows));
        out.insert(out.end(), std::make_move_iterator(decoded.begin()),
                  std::make_move_iterator(decoded.end()));
        if (!full_chunk) break;  // a short chunk is the last one
    }
    return out;
}

std::int64_t SessionDatabase::LastMessageSeq(const std::string& session_id) const {
    std::lock_guard lock(mu_);
    // ACTIVE transcript only: a rewound session's HistoryStore must seed its
    // seq accounting from the transcript the user sees, and the next append --
    // which reclaims the discarded tail in its own transaction -- continues
    // that sequence.
    CachedStmt query(db_, prepared_,
                     "SELECT MAX(seq) FROM messages WHERE session_id = ? AND discarded = 0;",
                     "failed to read last message seq");
    sqlite3_bind_text(query.stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
    // MAX() over any set returns EXACTLY ONE row -- NULL when the set is empty.
    // So `step() != SQLITE_ROW` here is never "no messages": it is a step
    // failure (BUSY/IOERR/CORRUPT) masquerading as -1, which the redo path then
    // read as "empty transcript" and LoadAfter(-1) turned into a FULL transcript
    // load -- duplicated history appended live. A failed read throws.
    if (const int rc = sqlite3_step(query.stmt); rc != SQLITE_ROW) {
        CheckRowLoopEnd(db_, rc, "failed to read last message seq");
    }
    if (sqlite3_column_type(query.stmt, 0) == SQLITE_NULL) {
        // MAX() over an empty set is NULL, not 0 -- and 0 is a real seq.
        return -1;
    }
    return sqlite3_column_int64(query.stmt, 0);
}

void SessionDatabase::SaveCompactionCheckpoint(const std::string& session_id,
                                               std::int64_t boundary_seq,
                                               const llm::Message& summary,
                                               const CompactionMeta& meta) {
    // Encode outside the mutex, same ruling as AppendMessage: the summary is
    // model output and dump() throws type_error.316 on invalid UTF-8, so the
    // replace handler is required -- but the DOM plus dump need not run while
    // every other session waits on mu_.
    const std::string content_json =
        MessageToJson(summary).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    std::lock_guard lock(mu_);

    // A new record per compaction. at_seq is read here, in the same statement,
    // from the ACTIVE transcript: the compaction ran after its last message.
    CachedStmt insert(db_, prepared_,
                      "INSERT INTO compactions (session_id, boundary_seq, at_seq,"
                      " summary_content_json, folded_messages, tokens_before, tokens_after,"
                      " trigger_kind, warning, skipped, active, created_at)"
                      " VALUES (?1, ?2,"
                      " COALESCE((SELECT MAX(seq) FROM messages"
                      "           WHERE session_id = ?1 AND discarded = 0), ?2),"
                      " ?3, ?4, ?5, ?6, ?7, ?9, ?10, ?11, ?8);",
                      "failed to save compaction checkpoint");
    sqlite3_bind_text(insert.stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert.stmt, 2, boundary_seq);
    // STATIC + explicit length, same reasoning as the append path: content can
    // be large, ~CachedStmt clears bindings, nothing retains the pointer.
    sqlite3_bind_text(insert.stmt, 3, content_json.data(), static_cast<int>(content_json.size()),
                      SQLITE_STATIC);
    sqlite3_bind_int64(insert.stmt, 4, meta.folded_messages);
    sqlite3_bind_int64(insert.stmt, 5, meta.tokens_before);
    sqlite3_bind_int64(insert.stmt, 6, meta.tokens_after);
    sqlite3_bind_text(insert.stmt, 7, meta.trigger.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert.stmt, 8, NextTimestamp());
    sqlite3_bind_text(insert.stmt, 9, meta.warning.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(insert.stmt, 10, meta.skipped ? 1 : 0);
    // A skipped record is a notice, never a checkpoint.
    sqlite3_bind_int(insert.stmt, 11, meta.skipped ? 0 : 1);
    if (sqlite3_step(insert.stmt) != SQLITE_DONE) {
        ThrowSqliteError(db_, "failed to save compaction checkpoint");
    }
}

void SessionDatabase::ClearCompactionCheckpoint(const std::string& session_id) {
    std::lock_guard lock(mu_);
    CachedStmt clear(db_, prepared_, "UPDATE compactions SET active = 0 WHERE session_id = ?;",
                     "failed to clear compaction checkpoint");
    sqlite3_bind_text(clear.stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(clear.stmt) != SQLITE_DONE) {
        ThrowSqliteError(db_, "failed to clear compaction checkpoint");
    }
}

std::optional<CompactionCheckpoint> SessionDatabase::LoadCompactionCheckpoint(
    const std::string& session_id) const {
    std::string json;
    CompactionCheckpoint out;
    {
        std::lock_guard lock(mu_);
        CachedStmt query(db_, prepared_,
                         "SELECT boundary_seq, summary_content_json, created_at, at_seq"
                         " FROM compactions WHERE session_id = ? AND active = 1"
                         " ORDER BY boundary_seq DESC, id DESC LIMIT 1;",
                         "failed to load compaction checkpoint");
        sqlite3_bind_text(query.stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
        const int rc = sqlite3_step(query.stmt);
        if (rc == SQLITE_DONE) return std::nullopt;  // no checkpoint: clean, not an error
        if (rc != SQLITE_ROW) ThrowSqliteError(db_, "failed to load compaction checkpoint");
        out.boundary_seq = sqlite3_column_int64(query.stmt, 0);
        json = reinterpret_cast<const char*>(sqlite3_column_text(query.stmt, 1));
        out.created_at_unix_ms = sqlite3_column_int64(query.stmt, 2);
        out.at_seq = sqlite3_column_int64(query.stmt, 3);
        // LIMIT 1, so exactly one row; confirm the scan is over rather than
        // trusting that (BUSY/IOERR must not read as "done").
        CheckRowLoopEnd(db_, sqlite3_step(query.stmt), "failed to load compaction checkpoint");
    }
    // Strict decode outside the lock, same as the message read paths: an
    // undecodable summary is corruption and must surface, not silently
    // truncate the resumed view.
    out.summary = JsonToMessage(nlohmann::json::parse(json));
    out.session_id = session_id;
    return out;
}

std::vector<CompactionRecord> SessionDatabase::LoadCompactionRecords(
    const std::string& session_id, std::int64_t from_seq, std::int64_t to_seq) const {
    struct Raw {
        CompactionRecord record;
        std::string json;
    };
    std::vector<Raw> raw;
    {
        std::lock_guard lock(mu_);
        CachedStmt query(db_, prepared_,
                         "SELECT boundary_seq, at_seq, summary_content_json, folded_messages,"
                         " tokens_before, tokens_after, trigger_kind, created_at, warning, skipped"
                         " FROM compactions WHERE session_id = ? AND at_seq >= ? AND at_seq <= ?"
                         " ORDER BY at_seq ASC, id ASC;",
                         "failed to load compaction records");
        sqlite3_bind_text(query.stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(query.stmt, 2, from_seq);
        sqlite3_bind_int64(query.stmt, 3, to_seq);
        int rc = SQLITE_ROW;
        while ((rc = sqlite3_step(query.stmt)) == SQLITE_ROW) {
            Raw row;
            row.record.boundary_seq = sqlite3_column_int64(query.stmt, 0);
            row.record.at_seq = sqlite3_column_int64(query.stmt, 1);
            row.json = reinterpret_cast<const char*>(sqlite3_column_text(query.stmt, 2));
            row.record.meta.folded_messages = sqlite3_column_int64(query.stmt, 3);
            row.record.meta.tokens_before = sqlite3_column_int64(query.stmt, 4);
            row.record.meta.tokens_after = sqlite3_column_int64(query.stmt, 5);
            if (const auto* trigger = sqlite3_column_text(query.stmt, 6)) {
                row.record.meta.trigger = reinterpret_cast<const char*>(trigger);
            }
            row.record.created_at_unix_ms = sqlite3_column_int64(query.stmt, 7);
            if (const auto* warning = sqlite3_column_text(query.stmt, 8)) {
                row.record.meta.warning = reinterpret_cast<const char*>(warning);
            }
            row.record.meta.skipped = sqlite3_column_int(query.stmt, 9) != 0;
            raw.push_back(std::move(row));
        }
        CheckRowLoopEnd(db_, rc, "failed to load compaction records");
    }
    std::vector<CompactionRecord> out;
    out.reserve(raw.size());
    for (auto& row : raw) {
        row.record.summary = JsonToMessage(nlohmann::json::parse(row.json));
        out.push_back(std::move(row.record));
    }
    return out;
}

std::vector<StoredMessage> SessionDatabase::LoadMessagesFrom(const std::string& session_id,
                                                             std::int64_t after_seq) const {
    // Same shape and strictness as LoadMessages -- the checkpoint-resume path
    // is exactly as damaged by a short read -- with the seq floor that keeps a
    // resumed long session's load proportional to the kept context.
    std::vector<RawRow> rows;
    {
        std::lock_guard lock(mu_);
        CachedStmt query(db_, prepared_,
                         "SELECT seq, content_json, token_count, created_at, agent_preset,"
                         " model FROM messages"
                         " WHERE session_id = ?1 AND discarded = 0 AND seq > ?2 ORDER BY seq ASC;",
                         "failed to load messages after seq");
        sqlite3_bind_text(query.stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(query.stmt, 2, after_seq);
        rows = CollectRows(query.stmt, db_, "failed to load messages after seq");
    }

    return DecodeRows(std::move(rows));
}

std::optional<StoredMessage> SessionDatabase::LoadMessage(const std::string& session_id,
                                                          std::int64_t seq) const {
    // Raw row collected under mu_, decoded OUTSIDE it -- the same discipline
    // every other read path follows (DecodeRows after the lock). This was the
    // one read that ran the JSON conversion under mu_, stalling every other
    // session behind a multi-hundred-KB tool result's parse.
    struct RawRow {
        std::string json;
        int token_count = 0;
        std::int64_t created_at = 0;
        MessageAttribution attribution;
    };
    std::optional<RawRow> raw;
    {
        std::lock_guard lock(mu_);
        // discarded = 0 in the predicate: a discarded seq IS found by this query
        // as "absent", which is exactly what the rewind path's validation wants --
        // a rewound-away position is not a rewindable one. Strict decode, same
        // contract as every other read path.
        CachedStmt query(db_, prepared_,
                         "SELECT content_json, token_count, created_at, agent_preset, model"
                         " FROM messages WHERE session_id = ?1 AND seq = ?2 AND discarded = 0;",
                         "failed to load message");
        sqlite3_bind_text(query.stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(query.stmt, 2, seq);
        const int rc = sqlite3_step(query.stmt);
        if (rc == SQLITE_DONE) return std::nullopt;
        if (rc != SQLITE_ROW) ThrowSqliteError(db_, "failed to load message");
        raw = RawRow{};
        const char* json_text = reinterpret_cast<const char*>(sqlite3_column_text(query.stmt, 0));
        raw->json = json_text != nullptr ? json_text : "";
        raw->token_count = sqlite3_column_int(query.stmt, 1);
        raw->created_at = sqlite3_column_int64(query.stmt, 2);
        raw->attribution.agent_preset = ColumnTextOrEmpty(query.stmt, 3);
        raw->attribution.model = ColumnTextOrEmpty(query.stmt, 4);
        // PRIMARY KEY is (id), UNIQUE is (session_id, seq): confirm the scan is
        // over rather than trusting that one row was all there was.
        CheckRowLoopEnd(db_, sqlite3_step(query.stmt), "failed to load message");
    }

    StoredMessage stored;
    stored.seq = seq;
    stored.message = JsonToMessage(nlohmann::json::parse(raw->json));
    stored.token_count = raw->token_count;
    stored.created_at_unix_ms = raw->created_at;
    stored.attribution = std::move(raw->attribution);
    return stored;
}

std::int64_t SessionDatabase::SetDiscardedAfter(const std::string& session_id,
                                                std::int64_t target_seq) {
    std::lock_guard lock(mu_);
    std::int64_t marked = 0;
    ExecChecked(db_, "BEGIN IMMEDIATE;", "failed to begin discard");
    try {
        // Idempotent by construction: rows already discarded from an earlier,
        // deeper rewind are not matched by discarded = 0, so moving the cursor
        // further back marks exactly the newly-discarded range.
        CachedStmt mark(db_, prepared_,
                        "UPDATE messages SET discarded = 1"
                        " WHERE session_id = ?1 AND seq >= ?2 AND discarded = 0;",
                        "failed to discard messages");
        sqlite3_bind_text(mark.stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(mark.stmt, 2, target_seq);
        if (sqlite3_step(mark.stmt) != SQLITE_DONE) {
            ThrowSqliteError(db_, "failed to discard messages");
        }
        marked = sqlite3_changes(db_);

        // message_count is the ACTIVE count now. Recomputed rather than
        // decremented by `marked`: one exact statement on a path a human
        // invoked beats two decrement rules that can drift, and the touch
        // keeps the picker's sort key honest about the session's activity.
        CachedStmt count(db_, prepared_,
                         "UPDATE sessions SET message_count ="
                         " (SELECT COUNT(*) FROM messages WHERE session_id = ?1 AND discarded = 0),"
                         " updated_at = ?2 WHERE id = ?1;",
                         "failed to recount session messages");
        sqlite3_bind_text(count.stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(count.stmt, 2, NextTimestamp());
        if (sqlite3_step(count.stmt) != SQLITE_DONE) {
            ThrowSqliteError(db_, "failed to recount session messages");
        }

        ExecChecked(db_, "COMMIT;", "failed to commit discard");
    } catch (...) {
        char* err = nullptr;
        if (sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, &err) != SQLITE_OK && err) {
            sqlite3_free(err);
        }
        throw;
    }
    return marked;
}

std::int64_t SessionDatabase::UnmarkDiscarded(const std::string& session_id) {
    std::lock_guard lock(mu_);
    std::int64_t restored = 0;
    ExecChecked(db_, "BEGIN IMMEDIATE;", "failed to begin restore");
    try {
        CachedStmt unmark(db_, prepared_,
                          "UPDATE messages SET discarded = 0"
                          " WHERE session_id = ?1 AND discarded = 1;",
                          "failed to restore discarded messages");
        sqlite3_bind_text(unmark.stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(unmark.stmt) != SQLITE_DONE) {
            ThrowSqliteError(db_, "failed to restore discarded messages");
        }
        restored = sqlite3_changes(db_);

        // Same recount as the mark path: the picker's count is the ACTIVE
        // count, whichever way the cursor last moved.
        CachedStmt count(db_, prepared_,
                         "UPDATE sessions SET message_count ="
                         " (SELECT COUNT(*) FROM messages WHERE session_id = ?1 AND discarded = 0),"
                         " updated_at = ?2 WHERE id = ?1;",
                         "failed to recount session messages");
        sqlite3_bind_text(count.stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(count.stmt, 2, NextTimestamp());
        if (sqlite3_step(count.stmt) != SQLITE_DONE) {
            ThrowSqliteError(db_, "failed to recount session messages");
        }

        ExecChecked(db_, "COMMIT;", "failed to commit restore");
    } catch (...) {
        char* err = nullptr;
        if (sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, &err) != SQLITE_OK && err) {
            sqlite3_free(err);
        }
        throw;
    }
    return restored;
}

SessionSummary SessionDatabase::ForkSession(const std::string& source_session_id,
                                            std::int64_t target_seq, const std::string& name,
                                            const std::optional<std::string>& agent_preset_override) {
    std::lock_guard lock(mu_);
    const std::int64_t now = NextTimestamp();
    const std::string id = GenerateUlid();
    const char* name_source = name.empty() ? "" : "explicit";
    int copied = 0;
    std::string project_root, agent_preset;
    // The source's persisted model overrides, read in the transaction below.
    // nullopt = the source row's column is NULL (no override), and the fork
    // row gets NULL too rather than an empty string.
    std::optional<std::string> source_model, source_subagent_model;
    // target_seq < 0: fork the WHOLE active transcript. The upper bound and
    // the checkpoint condition both open up -- "copy everything" has no
    // trailing user message to point at, and any existing checkpoint covers
    // part of what was copied by definition.
    const bool fork_all = target_seq < 0;
    const std::int64_t copy_upper =
        fork_all ? std::numeric_limits<std::int64_t>::max() : target_seq;

    ExecChecked(db_, "BEGIN IMMEDIATE;", "failed to begin fork");
    try {
        // Source validation inside the transaction: it must exist and must NOT
        // be a subagent. Subagent transcripts are one task-scoped turn, hidden
        // from the picker and refused rewind/resume -- forking one would hand a
        // top-level session a transcript the whole UI model refuses to show.
        Stmt source(db_, "SELECT project_root, agent_preset, model, subagent_model FROM sessions"
                         " WHERE id = ?1 AND parent_session_id IS NULL;",
                    "failed to read the source session");
        sqlite3_bind_text(source.stmt, 1, source_session_id.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(source.stmt) != SQLITE_ROW) {
            throw std::runtime_error("cannot fork '" + source_session_id +
                                     "': no such top-level session");
        }
        project_root = reinterpret_cast<const char*>(sqlite3_column_text(source.stmt, 0));
        agent_preset = reinterpret_cast<const char*>(sqlite3_column_text(source.stmt, 1));
        // The persisted model overrides ride along: a fork's whole point is
        // "try it on another model from HERE", and "here" includes the model
        // the user pinned with /model (v8) and /subagent-model (v10). Dropping
        // them forked a /model session onto its preset's default model --
        // a different starting point than the one the user was looking at.
        const bool have_source_model = sqlite3_column_type(source.stmt, 2) != SQLITE_NULL;
        if (have_source_model) {
            source_model = reinterpret_cast<const char*>(sqlite3_column_text(source.stmt, 2));
        }
        const bool have_source_subagent_model =
            sqlite3_column_type(source.stmt, 3) != SQLITE_NULL;
        if (have_source_subagent_model) {
            source_subagent_model =
                reinterpret_cast<const char*>(sqlite3_column_text(source.stmt, 3));
        }

        // Target validation, same shape as rewind's IsRewindTarget: the fork
        // point must be an ACTIVE user message with non-empty text, so a fork
        // lands exactly where a rewind would and never splits a tool-call pair.
        // Skipped when forking everything.
        if (!fork_all) {
        Stmt target(db_, "SELECT role, content_json FROM messages"
                         " WHERE session_id = ?1 AND seq = ?2 AND discarded = 0;",
                   "failed to read the fork target");
        sqlite3_bind_text(target.stmt, 1, source_session_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(target.stmt, 2, target_seq);
        if (sqlite3_step(target.stmt) != SQLITE_ROW) {
            throw std::runtime_error("fork target seq " + std::to_string(target_seq) +
                                     " is not an active message of '" + source_session_id + "'");
        }
        const std::string role = reinterpret_cast<const char*>(sqlite3_column_text(target.stmt, 0));
        const std::string content_json =
            reinterpret_cast<const char*>(sqlite3_column_text(target.stmt, 1));
        const llm::Message message =
            JsonToMessage(nlohmann::json::parse(content_json));  // throws on corruption
        bool text_bearing = false;
        if (message.role == llm::Role::User) {
            for (const auto& block : message.content) {
                if (const auto* text = std::get_if<llm::TextBlock>(&block)) {
                    if (!text->text.empty()) text_bearing = true;
                }
            }
        }
        if (!text_bearing) {
            throw std::runtime_error("fork target seq " + std::to_string(target_seq) +
                                     " is not a text-bearing user message");
        }
        }

        // Mint the child row: a top-level session with fork provenance. The
        // source's project_root/agent_preset and any persisted model overrides
        // carry over (the registry's CreateSession params can still override
        // the preset via its own resolve path).
        Stmt insert(db_, "INSERT INTO sessions (id, parent_session_id, name, name_source,"
                         " project_root, agent_preset, created_at, updated_at,"
                         " forked_from_session_id, model, subagent_model)"
                         " VALUES (?, NULL, ?, ?, ?, ?, ?, ?, ?, ?, ?);",
                    "failed to create forked session");
        sqlite3_bind_text(insert.stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert.stmt, 2, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert.stmt, 3, name_source, -1, SQLITE_STATIC);
        sqlite3_bind_text(insert.stmt, 4, project_root.c_str(), -1, SQLITE_TRANSIENT);
        const std::string& effective_preset = agent_preset_override ? *agent_preset_override
                                                                    : agent_preset;
        sqlite3_bind_text(insert.stmt, 5, effective_preset.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(insert.stmt, 6, now);
        sqlite3_bind_int64(insert.stmt, 7, now);
        sqlite3_bind_text(insert.stmt, 8, source_session_id.c_str(), -1, SQLITE_TRANSIENT);
        if (source_model) {
            sqlite3_bind_text(insert.stmt, 9, source_model->c_str(), -1, SQLITE_TRANSIENT);
        } else {
            sqlite3_bind_null(insert.stmt, 9);
        }
        if (source_subagent_model) {
            sqlite3_bind_text(insert.stmt, 10, source_subagent_model->c_str(), -1,
                              SQLITE_TRANSIENT);
        } else {
            sqlite3_bind_null(insert.stmt, 10);
        }
        if (sqlite3_step(insert.stmt) != SQLITE_DONE) {
            ThrowSqliteError(db_, "failed to create forked session");
        }

        // The copy: active rows below the target, SEQS VERBATIM. Seq spaces
        // are per-session (UNIQUE(session_id, seq)), reads are keyset-paged
        // within one session, and appends continue from MAX(seq)+1 -- nothing
        // requires a child's seq space to start at zero, and keeping the
        // source's numbers means a copied checkpoint needs no renumbering.
        Stmt copy(db_, "INSERT INTO messages (session_id, seq, role, content_json,"
                       " token_count, created_at, agent_preset, model)"
                       " SELECT ?1, seq, role, content_json, token_count, created_at,"
                       " agent_preset, model"
                       " FROM messages WHERE session_id = ?2 AND discarded = 0 AND seq < ?3;",
                  "failed to copy messages into the fork");
        sqlite3_bind_text(copy.stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(copy.stmt, 2, source_session_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(copy.stmt, 3, copy_upper);
        if (sqlite3_step(copy.stmt) != SQLITE_DONE) {
            ThrowSqliteError(db_, "failed to copy messages into the fork");
        }
        copied = sqlite3_changes(db_);

        // Compaction records ride along when their summary covers only part
        // of the copied prefix (boundary_seq < the copy bound); the others
        // summarize history the child does not have. A record that ran past
        // the bound is re-positioned to the child's last copied seq -- its
        // summary still describes that prefix. `active` is copied too: a
        // record the source had deactivated (/clear) must not become the
        // child's checkpoint.
        Stmt copy_compactions(db_, "INSERT INTO compactions (session_id, boundary_seq, at_seq,"
                                   " summary_content_json, folded_messages, tokens_before,"
                                   " tokens_after, trigger_kind, warning, skipped, active,"
                                   " created_at)"
                                   " SELECT ?1, boundary_seq, MIN(at_seq, ?3 - 1),"
                                   " summary_content_json,"
                                   " folded_messages, tokens_before, tokens_after, trigger_kind,"
                                   " warning, skipped, active, created_at"
                                   " FROM compactions WHERE session_id = ?2 AND boundary_seq < ?3"
                                   " ORDER BY id;",
                              "failed to copy the compaction records");
        sqlite3_bind_text(copy_compactions.stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(copy_compactions.stmt, 2, source_session_id.c_str(), -1,
                          SQLITE_TRANSIENT);
        sqlite3_bind_int64(copy_compactions.stmt, 3, copy_upper);
        if (sqlite3_step(copy_compactions.stmt) != SQLITE_DONE) {
            ThrowSqliteError(db_, "failed to copy the compaction records");
        }

        // Maintain the count the same way every other path does.
        Stmt count(db_, "UPDATE sessions SET message_count = ?2 WHERE id = ?1;",
                   "failed to set the forked session's message count");
        sqlite3_bind_text(count.stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(count.stmt, 2, copied);
        if (sqlite3_step(count.stmt) != SQLITE_DONE) {
            ThrowSqliteError(db_, "failed to set the forked session's message count");
        }

        ExecChecked(db_, "COMMIT;", "failed to commit fork");
    } catch (...) {
        char* err = nullptr;
        if (sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, &err) != SQLITE_OK && err) {
            sqlite3_free(err);
        }
        throw;
    }

    SessionSummary summary;
    summary.session_id = id;
    summary.name = name;
    summary.name_source = name_source;
    summary.project_root = std::move(project_root);
    // The EFFECTIVE preset -- the override when one was given -- so the row and
    // the summary always describe what this fork will actually run.
    summary.agent_preset =
        agent_preset_override ? *agent_preset_override : std::move(agent_preset);
    summary.created_at_unix_ms = now;
    summary.updated_at_unix_ms = now;
    summary.message_count = copied;
    summary.forked_from_session_id = source_session_id;
    // The overrides the fork row now carries: the request's preset override
    // already landed above; the source's model choices were copied verbatim
    // in the transaction.
    summary.model = std::move(source_model);
    summary.subagent_model = std::move(source_subagent_model);
    return summary;
}

std::filesystem::path ResolveSessionDbPath(const std::string& configured) {
    if (configured.empty()) return common::DataDir() / "sessions.db";
    const std::filesystem::path path(configured);
    if (path.is_absolute()) return path;
    return common::DataDir() / path;
}

}  // namespace minimal::session
