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

// One keyset page. messages is descending by seq; has_more is true when a row
// exists just past the page (the query fetches limit+1 and discards the probe).
struct MessagePage {
    std::vector<StoredMessage> messages;
    bool has_more = false;
};

}  // namespace minimal::session
