#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "minimal/session/session_database.hpp"
#include "minimal/session/stored_message.hpp"

namespace minimal::session {

// Read-only history access, the same path GetMessages will use. Plugins receive
// this through the ABI utilities half instead of touching SQLite directly; it is
// deliberately limited to reads -- writes go through a session's HistoryStore.
class SessionReader {
public:
    explicit SessionReader(SessionDatabase& db) : db_(db) {}

    // Keyset page, descending by seq. before_seq is an exclusive upper bound;
    // nullopt starts at the most recent message. Not a sentinel value, because
    // seq starts at 0 and any in-range sentinel would mean "before the first
    // message" to one caller and "no cursor" to another. limit must be > 0 (the
    // RPC layer applies settings.memory.scrollback_page_size as its default).
    MessagePage GetMessages(const std::string& session_id, std::optional<std::int64_t> before_seq,
                            int limit) const;

    // A real cursor, taken exactly so it does not fall into the deleted overload
    // below -- `seq` is int64_t and int64_t->int is a standard conversion, which
    // would otherwise outrank the user-defined conversion to optional.
    MessagePage GetMessages(const std::string& session_id, std::int64_t before_seq,
                            int limit) const {
        return GetMessages(session_id, std::optional<std::int64_t>(before_seq), limit);
    }

    // Deleted so `GetMessages(id, 0, n)` does not compile. It used to mean "from
    // the most recent" and now means "before seq 0", which is the empty page --
    // and `0` converts to an engaged optional silently, so the compiler would
    // otherwise accept every pre-existing call site and quietly change what it
    // did. Callers that mean "no cursor" say std::nullopt.
    MessagePage GetMessages(const std::string& session_id, int before_seq, int limit) const = delete;

    // Keyset page, ASCENDING: every active row with seq > after_seq -- the
    // delta-fetch path. has_more means more NEWER rows remain, not older
    // pages (see SessionDatabase::GetMessagesPageAfter).
    MessagePage GetMessagesAfter(const std::string& session_id, std::int64_t after_seq,
                                 int limit) const;

    // The full transcript, ascending by seq -- the one-shot load resume seeds
    // the loop's working history with when no compaction checkpoint exists.
    // Not a scrollback path: GetMessages pages precisely so reading a 1M-token
    // history stays bounded, and this is the only read allowed to materialize
    // the whole thing.
    std::vector<StoredMessage> LoadAll(const std::string& session_id) const;

    // The tail after `after_seq` (exclusive), ascending -- the checkpoint
    // resume path pairs this with the checkpoint's summary, so resuming a long
    // session loads the kept context, not the transcript it outgrew.
    std::vector<StoredMessage> LoadAfter(const std::string& session_id,
                                         std::int64_t after_seq) const;

    // One ACTIVE message by exact seq; nullopt when the row does not exist or
    // is discarded. The rewind path's target validation reads through here.
    std::optional<StoredMessage> LoadMessage(const std::string& session_id,
                                             std::int64_t seq) const;

    // MAX(seq) over the ACTIVE transcript, -1 when empty. The rewind event's
    // active_last_seq, and what a redo's restore range is computed from.
    std::int64_t ActiveLastSeq(const std::string& session_id) const;

private:
    SessionDatabase& db_;
};

}  // namespace minimal::session
