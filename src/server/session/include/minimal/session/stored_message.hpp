#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "minimal/llm/message.hpp"

namespace minimal::session {

// One row of the messages table, decoded. Mirrors proto StoredMessage: the
// scrollback RPC is a thin translation of this type.
struct StoredMessage {
    std::int64_t seq = 0;
    llm::Message message;
    int token_count = 0;
    std::int64_t created_at_unix_ms = 0;
};

// One keyset page. messages is descending by seq; has_more is true when a row
// exists just past the page (the query fetches limit+1 and discards the probe).
struct MessagePage {
    std::vector<StoredMessage> messages;
    bool has_more = false;
};

}  // namespace minimal::session
