#include "minimal/session/session_database.hpp"

#include <sqlite3.h>

#include <cctype>
#include <chrono>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <variant>

#include "minimal/common/config_paths.hpp"
#include "minimal/session/message_codec.hpp"
#include "minimal/session/ulid.hpp"

namespace minimal::session {
namespace {

std::int64_t NowMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

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
    }};
    return migrations;
}

[[noreturn]] void ThrowSqliteError(sqlite3* db, const std::string& context) {
    throw std::runtime_error(context + ": " + sqlite3_errmsg(db));
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

void ExecChecked(sqlite3* db, const char* sql, const std::string& context) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        const std::string message = err ? err : "unknown error";
        sqlite3_free(err);
        throw std::runtime_error(context + ": " + message);
    }
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
        if (sqlite3_step(stmt) == SQLITE_ROW) current = sqlite3_column_int(stmt, 0);
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
        char* err = nullptr;
        if (sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, &err) != SQLITE_OK && err) {
            sqlite3_free(err);
        }
        throw;
    }
}

// Collapses whitespace runs to single spaces and drops leading/trailing, so a
// multi-line prompt becomes a usable tab label. The c < 0x80 guard is what keeps
// this from splitting a UTF-8 sequence: continuation bytes are all > 127, and
// isspace() is locale-dependent above ASCII -- under a Latin-1 locale 0xA0 is a
// space, so an unguarded call would delete bytes out of the middle of a
// character. The server never calls setlocale today; this makes that not matter.
std::string CollapseWhitespace(std::string_view text) {
    std::string out;
    bool pending_space = false;
    for (const unsigned char c : text) {
        if (c < 0x80 && std::isspace(c)) {
            pending_space = true;
        } else {
            if (!out.empty() && pending_space) out += ' ';
            pending_space = false;
            out += static_cast<char>(c);
        }
    }
    return out;
}

// Cuts at a code-point boundary, never mid-sequence. A stray continuation byte
// counts as one character rather than desynchronizing the scan.
std::string TruncateToCodePoints(std::string_view s, int max_chars) {
    std::size_t chars = 0;
    std::size_t i = 0;
    while (i < s.size() && chars < static_cast<std::size_t>(max_chars)) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t len;
        if (c < 0x80) len = 1;
        else if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        else len = 1;
        if (i + len > s.size()) len = s.size() - i;
        i += len;
        ++chars;
    }
    return std::string(s.substr(0, i));
}

// The free core default name: the first user message's text, made label-shaped.
constexpr int kDefaultNameMaxChars = 60;
std::string SessionNameFromText(std::string_view text) {
    return TruncateToCodePoints(CollapseWhitespace(text), kDefaultNameMaxChars);
}

struct Stmt {
    sqlite3_stmt* stmt = nullptr;
    explicit Stmt(sqlite3* db, const char* sql, const std::string& context) {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            ThrowSqliteError(db, context);
        }
    }
    ~Stmt() {
        if (stmt) sqlite3_finalize(stmt);
    }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;
};

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

}  // namespace

const std::vector<Migration>& DefaultMigrations() { return DefaultMigrationsImpl(); }

SessionDatabase::SessionDatabase(const std::filesystem::path& db_path, const OpenOptions& options)
    : path_(db_path) {
    std::error_code ec;
    if (path_.has_parent_path()) {
        std::filesystem::create_directories(path_.parent_path(), ec);
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
    for (const char* pragma : {"PRAGMA journal_mode=WAL;", "PRAGMA foreign_keys=ON;",
                               "PRAGMA synchronous=NORMAL;", "PRAGMA busy_timeout=5000;"}) {
        ExecChecked(db_, pragma, std::string("failed to set ") + pragma);
    }

    const auto& migrations = options.migrations ? *options.migrations : DefaultMigrations();
    ApplyMigrations(db_, kSchemaVersion, migrations);
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
                                              std::string project_root, std::string agent_preset) {
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
        " agent_preset, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?);",
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
    if (sqlite3_step(insert.stmt) != SQLITE_DONE) {
        ThrowSqliteError(db_, "failed to create session");
    }

    return SessionSummary{std::move(id),          std::move(parent_session_id),
                          std::move(name),        name_source,
                          std::move(project_root), std::move(agent_preset),
                          now,                    now,
                          0};
}

std::int64_t SessionDatabase::AppendMessage(const std::string& session_id,
                                            const llm::Message& message, int token_count) {
    std::lock_guard lock(mu_);
    const std::int64_t now = NextTimestamp();
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
    const std::string content_json =
        MessageToJson(message).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    const char* role = message.role == llm::Role::User ? "user" : "assistant";

    // Name candidate for the free default: the first user message carrying text.
    // A tool-result-only user message (e.g. an injected subagent completion) is
    // not a name, so the search simply continues with later messages.
    std::string name_candidate;
    if (message.role == llm::Role::User) {
        for (const auto& block : message.content) {
            if (const auto* text = std::get_if<llm::TextBlock>(&block)) {
                name_candidate = SessionNameFromText(text->text);
                break;
            }
        }
    }

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
            "INSERT INTO messages (session_id, seq, role, content_json, token_count, created_at)"
            " VALUES (?, COALESCE((SELECT MAX(seq) FROM messages"
            " WHERE session_id = ? AND discarded = 0), -1) + 1,"
            " ?, ?, ?, ?);",
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

        // Only when the session still has no name: an explicit creation name or
        // a name an observer plugin set later is never clobbered. The source
        // is recorded with it (migration v6), so the rename seam can tell a
        // generated title from a default apart from a user's choice.
        if (!name_candidate.empty()) {
            CachedStmt rename(db_, prepared_,
                              "UPDATE sessions SET name = ?, name_source = 'default'"
                              " WHERE id = ? AND name = '';",
                              "failed to set default session name");
            sqlite3_bind_text(rename.stmt, 1, name_candidate.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(rename.stmt, 2, session_id.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(rename.stmt) != SQLITE_DONE) {
                ThrowSqliteError(db_, "failed to set default session name");
            }
        }

        ExecChecked(db_, "COMMIT;", "failed to commit append");
    } catch (...) {
        char* err = nullptr;
        if (sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, &err) != SQLITE_OK && err) {
            sqlite3_free(err);
        }
        throw;
    }
    return assigned_seq;
}

bool SessionDatabase::UpdateSessionName(const std::string& session_id, const std::string& name) {
    std::lock_guard lock(mu_);
    Stmt update(db_, "UPDATE sessions SET name = ?, name_source = 'observer', updated_at = ?"
                     " WHERE id = ?;",
                "failed to rename session");
    sqlite3_bind_text(update.stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(update.stmt, 2, NextTimestamp());
    sqlite3_bind_text(update.stmt, 3, session_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(update.stmt) != SQLITE_DONE) {
        ThrowSqliteError(db_, "failed to rename session");
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
    Stmt delete_stmt(db_, "DELETE FROM sessions WHERE id = ?;", "failed to delete session");
    sqlite3_bind_text(delete_stmt.stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(delete_stmt.stmt) != SQLITE_DONE) {
        ThrowSqliteError(db_, "failed to delete session");
    }
    return sqlite3_changes(db_) == 1;
}

std::optional<SessionSummary> SessionDatabase::GetSession(const std::string& session_id) const {
    std::lock_guard lock(mu_);
    // message_count is a maintained column (bumped in the append transaction,
    // backfilled by migration v2), not a correlated COUNT(*) -- the subquery
    // made every call pay O(messages-of-session) to fill a display field.
    CachedStmt query(db_, prepared_,
                     "SELECT s.id, s.parent_session_id, s.name, s.name_source, s.project_root,"
                     " s.agent_preset, s.created_at, s.updated_at, s.message_count"
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

    SessionSummary summary;
    summary.session_id = reinterpret_cast<const char*>(sqlite3_column_text(query.stmt, 0));
    if (sqlite3_column_type(query.stmt, 1) == SQLITE_TEXT) {
        summary.parent_session_id =
            reinterpret_cast<const char*>(sqlite3_column_text(query.stmt, 1));
    }
    summary.name = reinterpret_cast<const char*>(sqlite3_column_text(query.stmt, 2));
    summary.name_source = reinterpret_cast<const char*>(sqlite3_column_text(query.stmt, 3));
    summary.project_root = reinterpret_cast<const char*>(sqlite3_column_text(query.stmt, 4));
    summary.agent_preset = reinterpret_cast<const char*>(sqlite3_column_text(query.stmt, 5));
    summary.created_at_unix_ms = sqlite3_column_int64(query.stmt, 6);
    summary.updated_at_unix_ms = sqlite3_column_int64(query.stmt, 7);
    summary.message_count = sqlite3_column_int(query.stmt, 8);
    return summary;
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
    // The cursor is a PAIR, because the sort key is. updated_at alone is not
    // unique -- two sessions can share a millisecond -- so a cursor on it would
    // either skip or repeat rows at a page boundary. The row comparison
    // (updated_at, id) < (?, ?) in lexicographic form is what makes the page
    // boundary exact, and it matches ORDER BY updated_at DESC, id ASC.
    // message_count is the maintained column (see GetSession), not a correlated
    // COUNT(*) that would make the page cost O(total messages).
    CachedStmt query(db_, prepared_,
                     "SELECT s.id, s.parent_session_id, s.name, s.name_source, s.project_root,"
                     " s.agent_preset, s.created_at, s.updated_at, s.message_count"
                     " FROM sessions s"
                     " WHERE (?1 IS NULL OR s.project_root = ?1) AND (?2 OR s.parent_session_id IS NULL)"
                     " AND (?3 IS NULL OR s.updated_at < ?3 OR (s.updated_at = ?3 AND s.id > ?4))"
                     " ORDER BY s.updated_at DESC, s.id ASC"
                     " LIMIT ?5;",
                     "failed to list sessions");
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
        SessionSummary summary;
        summary.session_id = reinterpret_cast<const char*>(sqlite3_column_text(query.stmt, 0));
        if (sqlite3_column_type(query.stmt, 1) == SQLITE_TEXT) {
            summary.parent_session_id =
                reinterpret_cast<const char*>(sqlite3_column_text(query.stmt, 1));
        }
        summary.name = reinterpret_cast<const char*>(sqlite3_column_text(query.stmt, 2));
        summary.name_source = reinterpret_cast<const char*>(sqlite3_column_text(query.stmt, 3));
        summary.project_root = reinterpret_cast<const char*>(sqlite3_column_text(query.stmt, 4));
        summary.agent_preset = reinterpret_cast<const char*>(sqlite3_column_text(query.stmt, 5));
        summary.created_at_unix_ms = sqlite3_column_int64(query.stmt, 6);
        summary.updated_at_unix_ms = sqlite3_column_int64(query.stmt, 7);
        summary.message_count = sqlite3_column_int(query.stmt, 8);
        out.push_back(std::move(summary));
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
    std::lock_guard lock(mu_);
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
    CachedStmt query(
        db_, prepared_,
        "SELECT seq, content_json, token_count, created_at FROM messages"
        " WHERE session_id = ?1 AND discarded = 0 AND seq < COALESCE(?2, 9223372036854775807)"
        " ORDER BY seq DESC LIMIT ?3;",
        "failed to page messages");
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

    MessagePage page;
    int rc = SQLITE_OK;
    while ((rc = sqlite3_step(query.stmt)) == SQLITE_ROW) {
        if (static_cast<int>(page.messages.size()) == limit) {
            page.has_more = true;
            // Deliberately leaves rc == SQLITE_ROW: this loop ends early ON
            // PURPOSE when the probe row appears, which is not an error and must
            // not be checked as one.
            return page;
        }
        StoredMessage stored;
        stored.seq = sqlite3_column_int64(query.stmt, 0);
        const char* json_text = reinterpret_cast<const char*>(sqlite3_column_text(query.stmt, 1));
        // Strict decode: a row that cannot be parsed is surfaced here, at read
        // time, rather than smuggled to the client as garbage. The decoded
        // message is trusted wholesale; the role column exists for SQL-level
        // filtering, not as a second source of truth.
        stored.message = JsonToMessage(nlohmann::json::parse(json_text));
        stored.token_count = sqlite3_column_int(query.stmt, 2);
        stored.created_at_unix_ms = sqlite3_column_int64(query.stmt, 3);
        page.messages.push_back(std::move(stored));
    }
    // Reached only when the scan ran out of rows, so anything but DONE is a real
    // failure. Returning short here reported has_more = false, which renders a
    // truncated page as the top of the transcript.
    CheckRowLoopEnd(db_, rc, "failed to read message page");
    return page;
}

std::vector<StoredMessage> SessionDatabase::LoadMessages(const std::string& session_id) const {
    // Ascending, no limit: the caller (resume) needs the whole working history
    // in order. Same strict decode as the paging path -- an undecodable row is
    // corruption or a newer binary's work, and must surface as an error rather
    // than as a silently truncated history that would then be re-persisted.
    struct RawRow {
        std::int64_t seq;
        std::string json;
        int token_count;
        std::int64_t created_at;
    };
    std::vector<RawRow> rows;
    {
        std::lock_guard lock(mu_);
        CachedStmt query(db_, prepared_,
                         "SELECT seq, content_json, token_count, created_at FROM messages"
                         " WHERE session_id = ?1 AND discarded = 0 ORDER BY seq ASC;",
                         "failed to load messages");
        sqlite3_bind_text(query.stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
        int rc = SQLITE_OK;
        while ((rc = sqlite3_step(query.stmt)) == SQLITE_ROW) {
            RawRow row;
            row.seq = sqlite3_column_int64(query.stmt, 0);
            row.json = reinterpret_cast<const char*>(sqlite3_column_text(query.stmt, 1));
            row.token_count = sqlite3_column_int(query.stmt, 2);
            row.created_at = sqlite3_column_int64(query.stmt, 3);
            rows.push_back(std::move(row));
        }
        // THE RESUME PATH, and the most damaging place to return short: a
        // partial read becomes the loop's working history while
        // SqliteHistoryStore keeps appending at MAX(seq) + 1, so the session
        // runs on a transcript with a hole in it and nothing ever says so.
        CheckRowLoopEnd(db_, rc, "failed to load messages");
    }

    // Decode OUTSIDE the lock. The JSON parse is the expensive part of a large
    // resume, and holding the one database mutex through it would stall every
    // other session's appends and reads for the whole load.
    std::vector<StoredMessage> out;
    out.reserve(rows.size());
    for (auto& row : rows) {
        StoredMessage stored;
        stored.seq = row.seq;
        stored.message = JsonToMessage(nlohmann::json::parse(row.json));
        stored.token_count = row.token_count;
        stored.created_at_unix_ms = row.created_at;
        out.push_back(std::move(stored));
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
                                               const llm::Message& summary) {
    std::lock_guard lock(mu_);
    // REPLACE, exactly like the append path's error handler: the summary is
    // model output and dump() throws type_error.316 on invalid UTF-8.
    const std::string content_json =
        MessageToJson(summary).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);

    // Upsert, not INSERT: each successful compaction REPLACES the previous
    // checkpoint -- its covered range is a strict superset (the new summary
    // range always contains the old one, since compaction folds from the head)
    // and only the latest is ever read.
    CachedStmt upsert(db_, prepared_,
                      "INSERT INTO compactions (session_id, boundary_seq, summary_content_json,"
                      " created_at) VALUES (?, ?, ?, ?)"
                      " ON CONFLICT(session_id) DO UPDATE SET boundary_seq = excluded.boundary_seq,"
                      " summary_content_json = excluded.summary_content_json,"
                      " created_at = excluded.created_at;",
                      "failed to save compaction checkpoint");
    sqlite3_bind_text(upsert.stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(upsert.stmt, 2, boundary_seq);
    // STATIC + explicit length, same reasoning as the append path: content can
    // be large, ~CachedStmt clears bindings, nothing retains the pointer.
    sqlite3_bind_text(upsert.stmt, 3, content_json.data(), static_cast<int>(content_json.size()),
                      SQLITE_STATIC);
    sqlite3_bind_int64(upsert.stmt, 4, NextTimestamp());
    if (sqlite3_step(upsert.stmt) != SQLITE_DONE) {
        ThrowSqliteError(db_, "failed to save compaction checkpoint");
    }
}

void SessionDatabase::ClearCompactionCheckpoint(const std::string& session_id) {
    std::lock_guard lock(mu_);
    CachedStmt del(db_, prepared_, "DELETE FROM compactions WHERE session_id = ?;",
                   "failed to clear compaction checkpoint");
    sqlite3_bind_text(del.stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(del.stmt) != SQLITE_DONE) {
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
                         "SELECT boundary_seq, summary_content_json, created_at"
                         " FROM compactions WHERE session_id = ?;",
                         "failed to load compaction checkpoint");
        sqlite3_bind_text(query.stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
        const int rc = sqlite3_step(query.stmt);
        if (rc == SQLITE_DONE) return std::nullopt;  // no checkpoint: clean, not an error
        if (rc != SQLITE_ROW) ThrowSqliteError(db_, "failed to load compaction checkpoint");
        out.boundary_seq = sqlite3_column_int64(query.stmt, 0);
        json = reinterpret_cast<const char*>(sqlite3_column_text(query.stmt, 1));
        out.created_at_unix_ms = sqlite3_column_int64(query.stmt, 2);
        // session_id is a PRIMARY KEY, so exactly one row; confirm the scan is
        // over rather than trusting that (BUSY/IOERR must not read as "done").
        CheckRowLoopEnd(db_, sqlite3_step(query.stmt), "failed to load compaction checkpoint");
    }
    // Strict decode outside the lock, same as the message read paths: an
    // undecodable summary is corruption and must surface, not silently
    // truncate the resumed view.
    out.summary = JsonToMessage(nlohmann::json::parse(json));
    out.session_id = session_id;
    return out;
}

std::vector<StoredMessage> SessionDatabase::LoadMessagesFrom(const std::string& session_id,
                                                             std::int64_t after_seq) const {
    // Same shape and strictness as LoadMessages -- the checkpoint-resume path
    // is exactly as damaged by a short read -- with the seq floor that keeps a
    // resumed long session's load proportional to the kept context.
    struct RawRow {
        std::int64_t seq;
        std::string json;
        int token_count;
        std::int64_t created_at;
    };
    std::vector<RawRow> rows;
    {
        std::lock_guard lock(mu_);
        CachedStmt query(db_, prepared_,
                         "SELECT seq, content_json, token_count, created_at FROM messages"
                         " WHERE session_id = ?1 AND discarded = 0 AND seq > ?2 ORDER BY seq ASC;",
                         "failed to load messages after seq");
        sqlite3_bind_text(query.stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(query.stmt, 2, after_seq);
        int rc = SQLITE_OK;
        while ((rc = sqlite3_step(query.stmt)) == SQLITE_ROW) {
            RawRow row;
            row.seq = sqlite3_column_int64(query.stmt, 0);
            row.json = reinterpret_cast<const char*>(sqlite3_column_text(query.stmt, 1));
            row.token_count = sqlite3_column_int(query.stmt, 2);
            row.created_at = sqlite3_column_int64(query.stmt, 3);
            rows.push_back(std::move(row));
        }
        CheckRowLoopEnd(db_, rc, "failed to load messages after seq");
    }

    std::vector<StoredMessage> out;
    out.reserve(rows.size());
    for (auto& row : rows) {
        StoredMessage stored;
        stored.seq = row.seq;
        stored.message = JsonToMessage(nlohmann::json::parse(row.json));
        stored.token_count = row.token_count;
        stored.created_at_unix_ms = row.created_at;
        out.push_back(std::move(stored));
    }
    return out;
}

std::optional<StoredMessage> SessionDatabase::LoadMessage(const std::string& session_id,
                                                          std::int64_t seq) const {
    std::lock_guard lock(mu_);
    // discarded = 0 in the predicate: a discarded seq IS found by this query
    // as "absent", which is exactly what the rewind path's validation wants --
    // a rewound-away position is not a rewindable one. Strict decode, same
    // contract as every other read path.
    CachedStmt query(db_, prepared_,
                     "SELECT content_json, token_count, created_at FROM messages"
                     " WHERE session_id = ?1 AND seq = ?2 AND discarded = 0;",
                     "failed to load message");
    sqlite3_bind_text(query.stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(query.stmt, 2, seq);
    const int rc = sqlite3_step(query.stmt);
    if (rc == SQLITE_DONE) return std::nullopt;
    if (rc != SQLITE_ROW) ThrowSqliteError(db_, "failed to load message");
    StoredMessage stored;
    stored.seq = seq;
    const char* json_text = reinterpret_cast<const char*>(sqlite3_column_text(query.stmt, 0));
    stored.message = JsonToMessage(nlohmann::json::parse(json_text));
    stored.token_count = sqlite3_column_int(query.stmt, 1);
    stored.created_at_unix_ms = sqlite3_column_int64(query.stmt, 2);
    // PRIMARY KEY is (id), UNIQUE is (session_id, seq): confirm the scan is
    // over rather than trusting that one row was all there was.
    CheckRowLoopEnd(db_, sqlite3_step(query.stmt), "failed to load message");
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

std::filesystem::path ResolveSessionDbPath(const std::string& configured) {
    if (configured.empty()) return common::DataDir() / "sessions.db";
    const std::filesystem::path path(configured);
    if (path.is_absolute()) return path;
    return common::DataDir() / path;
}

}  // namespace minimal::session
