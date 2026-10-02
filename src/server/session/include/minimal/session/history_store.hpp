#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>

#include "minimal/llm/message.hpp"
#include "minimal/session/stored_message.hpp"

namespace minimal::session {

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
    //
    // attribution: the agent preset and model that produced an assistant
    // message; empty for everything else (same explicit-argument rule).
    virtual void Append(const llm::Message& message, std::optional<int> output_tokens,
                        const MessageAttribution& attribution) = 0;

    // A tool round's assistant message (carrying tool_use blocks) and the user
    // message answering every one of them, persisted as ONE unit. The pairing
    // invariant -- no tool_use without its results directly behind it -- is
    // enforced by construction: a store that can fail between the two writes
    // must override this to be atomic. The default (two Appends) is for
    // in-memory fakes that cannot fail midway.
    virtual void AppendRound(const llm::Message& assistant, std::optional<int> output_tokens,
                             const MessageAttribution& attribution,
                             const llm::Message& results) {
        Append(assistant, output_tokens, attribution);
        Append(results, std::nullopt, MessageAttribution{});
    }

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

    // Appends a compaction record; the one with the highest boundary_seq among
    // the active records is the checkpoint a resume seeds from. Every record
    // stays visible to scrollback (see CompactionRecord).
    virtual void SaveCompactionCheckpoint(std::int64_t boundary_seq, const llm::Message& summary,
                                          const CompactionMeta& meta) = 0;
    // Deactivates the checkpoint (resume goes back to the full transcript);
    // the records stay for scrollback -- the compactions did happen.
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

    // Sets the durable store's name for this session, but ONLY while it still
    // has none -- an explicit creation name or one an observer plugin set
    // later is never clobbered. Returns whether this call is the one that set
    // it, so the caller (agent::Session, which computes the candidate as a
    // domain rule -- see agent/session_naming.hpp) can tell "already named"
    // from "just named" without a second read. Default no-op / false: in-
    // memory test fakes have no durable row to name.
    virtual bool SetDefaultNameIfUnset(const std::string& name) {
        (void)name;
        return false;
    }

    // --- The session row's user-chosen settings ------------------------------
    // Durable records a later resume reads back. May throw on a store failure;
    // the caller decides whether that is worth more than a log line. Defaults
    // are the in-memory answer: nothing durable to write, so nothing fails.

    // A user rename (/rename): unconditional -- no guard against clobbering,
    // the user's choice wins -- and marked explicit so later automatic naming
    // skips the session. False when the row could not be renamed.
    virtual bool Rename(const std::string& name) {
        (void)name;
        return true;
    }
    // The pinned "<provider>/<model>" (/model, --model); nullopt clears it.
    virtual void PersistModelOverride(const std::optional<std::string>& provider_model) {
        (void)provider_model;
    }
    // The pinned model for this session's subagents (/subagent-model).
    virtual void PersistSubagentModel(const std::optional<std::string>& provider_model) {
        (void)provider_model;
    }
};

}  // namespace minimal::session
