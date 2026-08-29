#pragma once

#include <string>

namespace minimal::client {

// Owns one gRPC stream per session and demultiplexes ServerEvents into the right
// per-tab SessionState by session_id. One client holds several of these: the
// focused session, plus any subagent children.
//
// Attaching to a child session on SubagentStartedEvent is a PROTOCOL OBLIGATION,
// not a TUI behavior -- a never-attached session is terminated by the server's
// initial-attach guard. The headless -p client must attach too.
class SessionClient {
public:
    explicit SessionClient(std::string server_address);
    ~SessionClient();

    SessionClient(const SessionClient&) = delete;
    SessionClient& operator=(const SessionClient&) = delete;

private:
    std::string server_address_;
};

}  // namespace minimal::client
