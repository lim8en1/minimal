#pragma once

#include <cstdint>
#include <optional>

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
};

}  // namespace minimal::agent
