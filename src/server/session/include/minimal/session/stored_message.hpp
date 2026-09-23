#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "minimal/llm/message.hpp"

namespace minimal::session {

// Which agent preset and model produced a message. Stamped on ASSISTANT rows
// only (the loop's effective preset at append time -- a per-turn override
// included); user and tool-result rows carry none. Empty = unknown, which is
// also what every pre-v9 row reads back as.
struct MessageAttribution {
    std::string agent_preset;
    std::string model;   // "<provider>/<model>"
};

// One row of the messages table, decoded. Mirrors proto StoredMessage: the
// scrollback RPC is a thin translation of this type.
struct StoredMessage {
    std::int64_t seq = 0;
    llm::Message message;
    int token_count = 0;
    std::int64_t created_at_unix_ms = 0;
    MessageAttribution attribution;
};

// What a compaction reports alongside its checkpoint, for the scrollback row
// a client renders. The token figures are whole-context sizes, both the
// loop's bytes/4 estimate (one basis, so before/after compare).
struct CompactionMeta {
    std::int64_t folded_messages = 0;
    std::int64_t tokens_before = 0;
    std::int64_t tokens_after = 0;
    std::string trigger;  // "auto" | "manual"
    // A guard's message (see AgentLoop::MaybeCompact). skipped = nothing was
    // folded: the record is a notice only, never a checkpoint.
    std::string warning;
    bool skipped = false;
};

// One compaction, as scrollback shows it. at_seq is the last ACTIVE message
// seq when it ran -- the row renders right after that message; boundary_seq
// is the last seq its summary replaced.
struct CompactionRecord {
    std::int64_t boundary_seq = 0;
    std::int64_t at_seq = 0;
    llm::Message summary;
    CompactionMeta meta;
    std::int64_t created_at_unix_ms = 0;
};

// One keyset page. messages is descending by seq; has_more is true when a row
// exists just past the page (the query fetches limit+1 and discards the probe).
// compactions are the records whose at_seq falls inside the page's seq range
// (ascending by at_seq), so a client interleaves them without a second call.
struct MessagePage {
    std::vector<StoredMessage> messages;
    bool has_more = false;
    std::vector<CompactionRecord> compactions;
};

}  // namespace minimal::session
