#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "minimal/llm/message.hpp"
#include "minimal/session/stored_message.hpp"

struct sqlite3;      // opaque handle; <sqlite3.h> stays private to the .cpp
struct sqlite3_stmt;

namespace minimal::session {

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
// Each AppendMessage is one transaction: the insert, the default-name set, and
// the updated_at touch commit together or not at all. A crash or a failed write
// therefore leaves an exact prefix of the transcript -- never a torn row.
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
    bool DeleteSession(const std::string& session_id);  // cascades to messages and children
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
    // accounting from the transcript the user actually sees.
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
    mutable std::mutex mu_;
    std::filesystem::path path_;
    std::int64_t last_ts_ = 0;  // guarded by mu_
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
