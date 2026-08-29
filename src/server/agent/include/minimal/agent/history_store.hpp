#pragma once

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
    virtual void Append(const llm::Message& message) = 0;
};

}  // namespace minimal::agent
