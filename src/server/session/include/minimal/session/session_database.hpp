#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "minimal/llm/message.hpp"
#include "minimal/session/stored_message.hpp"

struct sqlite3;      // opaque handle; <sqlite3.h> stays private to the .cpp
struct sqlite3_stmt;

namespace minimal::common {
class ExclusiveFileLock;  // the <db>.lock guard; held open for the process lifetime
}  // namespace minimal::common

namespace minimal::session {

// The two keyset-paging statements, exported VERBATIM. The EXPLAIN invariant
// tests assert the planner's plan for the statements that actually run --
// transcribing the SQL into a test produces a lookalike that drifts silently
// the moment the statement changes, which is how a reverted predicate passed
// its own guard.
inline constexpr std::string_view kMessagesPageSql =
    "SELECT seq, content_json, token_count, created_at FROM messages"
    " WHERE session_id = ?1 AND discarded = 0 AND seq < COALESCE(?2, 9223372036854775807)"
    " ORDER BY seq DESC LIMIT ?3;";

// The session picker's page. The cursor is a PAIR because the sort key is:
// updated_at is not unique, and ordering is (updated_at DESC, id ASC). The
// SEEK is carried by the `updated_at <= COALESCE(?3, INT64_MAX)` ceiling --
// with no cursor it folds to `<= INT64_MAX`, with one it bounds the range --
// while the OR below only disambiguates the tie at the cursor, exactly like
// the old `(updated_at = ?3 AND s.id > ?4)` term. The previous
// `?3 IS NULL OR ...` prefix defeated the planner: every page request
// re-scanned the whole index from the top past every newer session, so paging
// the picker was O(n) per page and O(n^2) total. `?4 IS NOT NULL` preserves
// the old semantics of a cursor missing its id half (the tie tier is
// excluded), and `COALESCE(?4, '')` was rejected because it would silently
// include that tier instead.
inline constexpr std::string_view kSessionsPageSql =
    "SELECT s.id, s.parent_session_id, s.name, s.name_source, s.project_root,"
    " s.agent_preset, s.created_at, s.updated_at, s.message_count"
    " FROM sessions s"
    " WHERE (?1 IS NULL OR s.project_root = ?1) AND (?2 OR s.parent_session_id IS NULL)"
    " AND s.updated_at <= COALESCE(?3, 9223372036854775807)"
    " AND (s.updated_at < COALESCE(?3, 9223372036854775807)"
    "      OR (?4 IS NOT NULL AND s.updated_at = ?3 AND s.id > ?4))"
    " ORDER BY s.updated_at DESC, s.id ASC LIMIT ?5;";

// One schema change: the user_version it establishes plus the statements that
// establish it. Applied in order; all pending migrations run in a single
// transaction, so a failure leaves the database exactly as it was opened.
struct Migration {
    int version = 0;
    std::vector<std::string> statements;
};

const std::vector<Migration>& DefaultMigrations();

struct SessionSummary {
    std::string session_id;
    std::optional<std::string> parent_session_id;  // set for subagents
    std::string name;
    // Where the name came from: 'explicit' (the user named it at creation),
    // 'default' (the free first-user-text rule), 'observer' (a plugin set it
    // later), or '' (pre-v6 rows). See IsExplicitlyNamed.
    std::string name_source;
    std::string project_root;
    std::string agent_preset;
    std::int64_t created_at_unix_ms = 0;
    std::int64_t updated_at_unix_ms = 0;  // last activity; the picker's sort key
    int message_count = 0;
};

// The persisted compaction checkpoint: the current derived VIEW of a session's
// context, cached so a resume reconstructs the working set the live session had
// -- summary + everything after boundary_seq -- instead of re-summarizing from
// the full uncompacted transcript. NOT part of the conversation: `messages`
// stays the exact record of what was said, and this row is a cache of a view
// over it. One row per session, latest-wins; the prefix it covers is immutable
// (appends only), so no invalidation exists beyond /clear and session deletion.
struct CompactionCheckpoint {
    std::string session_id;
    // The summary covers every message with seq <= boundary_seq; the working
    // set resumes with seq > boundary_seq.
    std::int64_t boundary_seq = 0;
    llm::Message summary;  // the synthetic User message at the head of the view
    std::int64_t created_at_unix_ms = 0;
};

struct ListSessionsFilter {
    std::optional<std::string> project_root;  // nullopt = all dirs (include_all_dirs)
    bool include_subagents = false;           // subagent rows are hidden by default

    // Bounded, because this grows forever otherwise: every session ever created,
    // each carrying its own COUNT(*) over the messages table, materialized into a
    // vector and then into a repeated proto field. GetMessages was carefully
    // keyset-paginated; the query beside it that also grows without bound was
    // not. Ordering is (updated_at DESC, id ASC), so the cursor is a pair.
    int limit = 0;                                  // <= 0 means no limit
    std::optional<std::int64_t> before_updated_at;  // exclusive, with the id below
    std::optional<std::string> before_id;
};

// OpenOptions is a test seam: overriding the migration list lets tests inject a
// failing migration and assert the transactional-rollback invariant without any
// fault-injection machinery in production code.
struct OpenOptions {
    std::optional<std::vector<Migration>> migrations;
};

// The process-wide session store: one SQLite connection, one mutex.
//
// Every public method takes the lock, so concurrent session workers appending
// to different sessions serialize on it. That is deliberate -- an append is a
// sub-millisecond transaction against second-scale LLM turns, so contention is
// negligible, and a single writer removes every intra-process locking question
// SQLite would otherwise raise. WAL plus busy_timeout still protect
// out-of-process readers (the startup sanity check, any future CLI).
//
// Expensive pure computation over caller data runs OUTSIDE the lock; only the
// transaction and the timestamp allocation serialize. The JSON encode of an
// append (and of a compaction summary) happens before the lock is taken, and
// the JSON decode of a read happens after it is released -- holding mu_ through
// a multi-hundred-KB tool result's encode stalled every other session for the
// whole conversion. Each AppendMessage is one transaction: the insert, the
// default-name set, and the updated_at touch commit together or not at all. A
// crash or a failed write therefore leaves an exact prefix of the transcript --
// never a torn row.
class SessionDatabase {
public:
    static constexpr int kSchemaVersion = 6;

    // Opens (creating if needed) and migrates. Throws std::runtime_error on any
    // failure, including a database whose user_version is newer than this
    // binary understands -- that is fatal by design, not best-effort.
    explicit SessionDatabase(const std::filesystem::path& db_path, const OpenOptions& options = {});
    ~SessionDatabase();

    SessionDatabase(const SessionDatabase&) = delete;
    SessionDatabase& operator=(const SessionDatabase&) = delete;

    // Generates the session id (ULID). An unknown parent_session_id is an error.
    SessionSummary CreateSession(std::string name, std::optional<std::string> parent_session_id,
                                 std::string project_root, std::string agent_preset);

    // One durable transcript row. Assigns the next per-session seq over the
    // ACTIVE transcript and returns it; sets the default name (truncated first
    // user text) when the session still has none; touches updated_at.
    // token_count is the assistant output count, 0 otherwise.
    //
    // THE FORK: the first append after a rewind reclaims the discarded tail --
    // the DELETE rides this transaction, so a crash before it keeps redo alive
    // and a crash after it leaves the fork standing, and the seq this method
    // assigns always continues the active sequence rather than landing above
    // rows the user rewound away.
    //
    // The seq is read back inside the transaction rather than computed by the
    // caller, because SQL is what assigns it -- anything else would be a second
    // opinion that the UNIQUE(session_id, seq) constraint exists to reject.
    std::int64_t AppendMessage(const std::string& session_id, const llm::Message& message,
                               int token_count = 0);

    bool UpdateSessionName(const std::string& session_id, const std::string& name);

    // True when the session was created WITH an explicit name
    // (CreateSessionRequest.name) rather than receiving the free default or a
    // later observer title. The rename seam consults this so an LLM titler can
    // never clobber a name the user chose. Pre-v6 rows backfill as '' -- "not
    // known to be explicit" -- which is the safe reading: those sessions are
    // old, and a titler only ever fires for a fresh session's first turn in
    // the process that created it.
    bool IsExplicitlyNamed(const std::string& session_id) const;
    // Deletes the session, every descendant, their messages, and their
    // compaction checkpoints. Messages go in CHUNKED sub-transactions (the
    // whole-transcript cascade used to hold mu_ for the entire delete);
    // returns true when the session existed. Deliberate tradeoff: a crash
    // mid-delete leaves the session row with a partial transcript rather than
    // all-or-nothing -- delete again to finish; the constructor's orphan sweep
    // cleans what a crash can orphan.
    bool DeleteSession(const std::string& session_id);

    // Checkpointing discipline. wal_autocheckpoint is DISABLED at open: the
    // default (checkpoint after every ~1000 WAL pages) made a committing append
    // synchronously absorb a checkpoint under mu_ -- tens of ms typical,
    // seconds on slow storage -- precisely on the tool-result path where the
    // payload is largest. Instead the session registry's reaper thread calls
    // MaybeRunPassiveCheckpoint() on the tick it already runs; a RESTART
    // checkpoint (PASSIVE never resets the log, which would grow without
    // bound) executes only once kCheckpointAppends appends have accumulated,
    // and a busy checkpoint loses nothing -- the frames stay; the next attempt
    // picks them up. Returns whether a checkpoint actually ran.
    static constexpr int kCheckpointAppends = 200;
    bool MaybeRunPassiveCheckpoint();

    // The wal_autocheckpoint value this connection runs with (0 = the write
    // path never self-checkpoints; the reaper owns it, see above). The pragma
    // is PER-CONNECTION, not persisted -- a fresh connection reports SQLite's
    // default, which is why the invariant test cannot RawScalar the file.
    int wal_autocheckpoint() const;

    // nullopt when the session does not exist. A STORE FAILURE (locked database,
    // I/O error, corruption) throws -- same ruling as SessionExists: answering
    // "no such session" for a session that is on disk is the one answer the
    // resume path cannot recover from.
    std::optional<SessionSummary> GetSession(const std::string& session_id) const;

    // Existence only. GetSession carries a COUNT(*) subquery over the whole
    // messages table, so using it as a presence check made every scrollback page
    // pay an O(n) count before the keyset seek that was supposed to make paging
    // independent of transcript length.
    bool SessionExists(const std::string& session_id) const;
    std::vector<SessionSummary> ListSessions(const ListSessionsFilter& filter) const;

    // Keyset page, descending by seq. before_seq is an exclusive upper bound;
    // nullopt means "start at the most recent". limit must be > 0.
    MessagePage GetMessagesPage(const std::string& session_id,
                                std::optional<std::int64_t> before_seq, int limit) const;

    // The session's entire transcript, ascending by seq. One-shot O(n) load --
    // resume is its only caller: the loop's working history is what gets sent
    // every turn, so a resumed session must hold it in memory exactly as a live
    // one does, and compaction bounds it from then on. Scrollback deliberately
    // never calls this; it pages.
    std::vector<StoredMessage> LoadMessages(const std::string& session_id) const;

    // The transcript tail after `after_seq` (exclusive), ascending. The
    // checkpoint-resume path: loading [checkpoint summary + this tail] keeps a
    // resumed long session's load proportional to the KEPT context, not to the
    // full transcript it outgrew. Same strictness contract as LoadMessages --
    // the most damaging place to return short is the working history.
    std::vector<StoredMessage> LoadMessagesFrom(const std::string& session_id,
                                                std::int64_t after_seq) const;

    // One ACTIVE message by exact seq; nullopt when the row does not exist or
    // is discarded. The rewind path's target validation reads through here: a
    // discarded or absent seq is not a rewindable position.
    std::optional<StoredMessage> LoadMessage(const std::string& session_id,
                                             std::int64_t seq) const;

    // MAX(seq) over the ACTIVE transcript, -1 when there are none. Discarded
    // rows are excluded so a resumed session's HistoryStore seeds its seq
    // accounting from the transcript the user actually sees. -1 means EMPTY,
    // never failed: a step failure throws, because -1 fed to LoadAfter(-1)
    // loads the entire transcript as "new" -- duplicated history.
    std::int64_t LastMessageSeq(const std::string& session_id) const;

    // --- Session traversal (rewind) ------------------------------------------

    // Marks every ACTIVE message with seq >= target_seq discarded. Returns how
    // many rows changed state (>= 1 for a valid target, which is itself
    // active). message_count is recomputed over the active set in the same
    // transaction, so the picker never counts a discarded tail.
    std::int64_t SetDiscardedAfter(const std::string& session_id, std::int64_t target_seq);

    // Restores every discarded message of this session. Returns how many rows
    // came back; zero means nothing to redo. The physical reclaim happens in
    // AppendMessage (see THE FORK above), so a zero here is final.
    std::int64_t UnmarkDiscarded(const std::string& session_id);

    // --- Compaction checkpoints (see CompactionCheckpoint above) -------------

    // Upsert: one checkpoint per session, the latest compaction's. Throws on
    // I/O failure -- the CALLER (AgentLoop::Compact) treats that as an
    // optimization miss and carries on, because the transcript itself is
    // unaffected either way.
    void SaveCompactionCheckpoint(const std::string& session_id, std::int64_t boundary_seq,
                                  const llm::Message& summary);

    // /clear drops the working set; a checkpoint surviving it would resurrect
    // a summary of a conversation the user deliberately wiped from view.
    void ClearCompactionCheckpoint(const std::string& session_id);

    // nullopt = no checkpoint (a session that never compacted, or one from
    // before checkpoints existed -- those resume the old way, from the full
    // transcript).
    std::optional<CompactionCheckpoint> LoadCompactionCheckpoint(
        const std::string& session_id) const;

    // See SessionReader::GetMessages for both of these. A real cursor is taken
    // exactly; a bare `0` is rejected, because it used to mean "from the most
    // recent" and now means "before seq 0" -- and the implicit conversion would
    // silently rewrite every old call site rather than failing one.
    MessagePage GetMessagesPage(const std::string& session_id, std::int64_t before_seq,
                                int limit) const {
        return GetMessagesPage(session_id, std::optional<std::int64_t>(before_seq), limit);
    }
    MessagePage GetMessagesPage(const std::string& session_id, int before_seq,
                                int limit) const = delete;

    const std::filesystem::path& path() const { return path_; }

private:
    // Strictly increasing even when operations land in the same millisecond.
    // updated_at is the picker's sort key, and a tie would fall back to id
    // order -- random within a millisecond, since ULIDs break same-ms ties with
    // their entropy suffix. Bumping by 1 keeps activity order total; the drift
    // from wall clock is at most one ms per operation, which is invisible.
    std::int64_t NextTimestamp();

    sqlite3* db_ = nullptr;  // opaque here; the .cpp includes <sqlite3.h>
    // Held FIRST (acquired in the constructor before sqlite3_open) and
    // released LAST (declared before db_, destroyed after it): the database is
    // never open while the lock file does not name this process. Unique_ptr
    // because <fcntl.h> stays out of this header.
    std::unique_ptr<common::ExclusiveFileLock> lock_;
    mutable std::mutex mu_;
    std::filesystem::path path_;
    std::int64_t last_ts_ = 0;  // guarded by mu_
    // Appends committed since the last checkpoint attempt (or open). Written
    // by AppendMessage's callers, read by the reaper; the counter alone must
    // not need mu_.
    std::atomic<int> appends_since_checkpoint_{0};
    // Cached prepared statements for the hot paths (appends, paging). Every
    // public method holds mu_, so the cache needs no lock of its own, and it is
    // mutable because the read paths (GetMessagesPage, GetSession) prepare on
    // first use too. Finalized in the destructor, before the connection closes.
    mutable std::unordered_map<std::string, sqlite3_stmt*> prepared_;  // guarded by mu_
};

// settings.json session_db_path -> concrete file. Empty means the default,
// DataDir()/sessions.db; a relative value resolves against DataDir() (never
// the cwd -- the server's cwd is not a stable concept); absolute is used as-is.
std::filesystem::path ResolveSessionDbPath(const std::string& configured);

}  // namespace minimal::session
