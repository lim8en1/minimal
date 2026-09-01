#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>

#include "minimal/llm/message.hpp"

namespace minimal::agent {

// Append-only, deliberately.
//
// The loop holds the compacted working set in memory -- it must, since that is
// what gets sent every turn, and compaction is what bounds it -- while the store
// is the durable full record that scrollback pages against. So the loop never
// reads back, and there is no Load(): returning full history would be an O(n)
// copy per turn against a record that may run to a million tokens.
//
// Writes happen mid-turn (user message, assistant message, each tool result)
// rather than once at the end, for two reasons: a crash mid-tool-sequence should
// not lose completed work, and the client reads scrollback via GetMessages while
// a long turn is still running.
class HistoryStore {
public:
    virtual ~HistoryStore() = default;

    // output_tokens is the backend-reported output count for an assistant
    // message (its row's token_count). User messages and tool results carry no
    // per-message count -- input_tokens covers the whole history, so there is
    // nothing attributable to one row -- and pass nullopt.
    //
    // NO DEFAULT ARGUMENT, deliberately. A default on a virtual is resolved from
    // the STATIC type of the expression, not the dynamic one, so a base and an
    // override that disagree silently produce different values depending on
    // which reference you happen to be holding. Callers pass it explicitly.
    virtual void Append(const llm::Message& message, std::optional<int> output_tokens) = 0;

    // The seq the last Append was assigned, or -1 if nothing has been appended.
    // Read, not counted: seq is assigned by the store, so a caller that tracked
    // its own count would be right only for a session that started empty. It is
    // what TurnSummary's first_seq/last_seq bounds are computed from.
    virtual std::int64_t last_appended_seq() const = 0;

    // --- Compaction checkpoint (the persisted VIEW, not history) -------------
    //
    // Compaction never touches `messages`; what it produces -- one summary
    // covering everything through boundary_seq -- is cached here so a RESUME
    // reconstructs [summary + tail] instead of re-summarizing the full
    // uncompacted transcript. The loop calls Save after every successful
    // Compact and Clear from /clear; session deletion cascades at the SQL
    // layer.

    // Upsert: the latest compaction replaces any earlier one.
    virtual void SaveCompactionCheckpoint(std::int64_t boundary_seq,
                                          const llm::Message& summary) = 0;
    virtual void ClearCompactionCheckpoint() = 0;

    // --- Session traversal (rewind) ------------------------------------------
    //
    // Conversation rewind marks a tail of the durable transcript DISCARDED
    // rather than deleting it. The mark is what keeps redo possible; the fork
    // (the first real append after the rewind) performs the physical reclaim
    // inside its own transaction -- SessionDatabase::AppendMessage does it, so
    // the discard is exactly-once with the turn that replaces the branch and
    // the seq space stays contiguous over the ACTIVE transcript.
    //
    // Deliberately not part of the minimal core above: a store that cannot
    // traverse (a test fake, a future non-SQLite backend) reports
    // SupportsRewind() == false and Session refuses the request with a clear
    // error instead of pretending to have honoured it.
    virtual bool SupportsRewind() const { return false; }

    // Marks every ACTIVE message with seq >= target_seq discarded; returns how
    // many rows changed state. The target's validity is the caller's problem:
    // Session validates it against the read side before coming here.
    virtual std::int64_t SetDiscardedAfter(std::int64_t target_seq) {
        (void)target_seq;
        throw std::logic_error("this history store does not support rewind");
    }

    // Restores every discarded message of this session; returns how many rows
    // came back. Zero means there was nothing to restore -- either nothing was
    // ever discarded or the fork already reclaimed it -- and the caller
    // answers that as "nothing to redo".
    virtual std::int64_t UnmarkDiscarded() {
        throw std::logic_error("this history store does not support rewind");
    }

    // The name the durable store currently has for this session, if the store
    // knows one. The SQLite store answers from the sessions row, which the free
    // default-naming rule inside its append transaction may have set AFTER the
    // Session was built from an empty name -- this is how the session learns the
    // name its transcript was given, and emits the SessionMetadataEvent the
    // client's tab label refreshes from. nullopt is "no news" (in-memory test
    // fakes), never an error.
    virtual std::optional<std::string> StoredName() const { return std::nullopt; }
};

}  // namespace minimal::agent
