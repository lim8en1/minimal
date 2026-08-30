#pragma once

// `minimal`'s command line, parsed as a value.
//
// Separated from main() because argv handling is exactly the kind of code that
// is never exercised until a user types something slightly unusual -- and when
// it went wrong here it went wrong SILENTLY: `-p` took whatever token came next
// even when that token was another flag, and unrecognised arguments were dropped
// without a word. `minimal -p --server unix:///x "do the thing"` therefore sent
// the model the prompt "--server", ignored the target, ignored the real prompt,
// and reported that it could not reach the default socket. Every part of that is
// individually plausible, which is what made it take a while to see.
//
// Two rules follow, and they are the whole design:
//   1. A value-taking flag never accepts a flag-shaped value. Use --flag=value
//      when the value legitimately starts with a dash.
//   2. Anything unrecognised is an error. Silence is what let rule 1 hide.

#include <string>
#include <string_view>
#include <vector>

namespace minimal::client {

struct HeadlessOptions {
    std::string prompt;
    std::string server;  // already a gRPC target; empty means the default socket
    std::string agent_preset;
    bool jsonl = false;
    bool bypass = false;
};

struct ParsedArgs {
    enum class Action { Tui, Headless, Usage, Version, Error };

    Action action = Action::Tui;
    HeadlessOptions headless;
    // Set when action == Error. Already formatted for stderr, possibly several
    // lines: the second line is usually the fix, which is the part that was
    // missing when this silently did the wrong thing.
    std::string error;
};

ParsedArgs ParseArgs(int argc, const char* const* argv);

// A --server value to a gRPC target.
//
// Accepts a bare filesystem path, because `minimal-server --listen` takes one
// and having the two halves of the same pair disagree is a trap: --listen
// /tmp/x.sock pairs with --server unix:///tmp/x.sock, and getting it wrong
// produced a gRPC resolver error naming neither flag. A value that already has a
// scheme, or that looks like host:port, is passed through untouched.
std::string ResolveServerTarget(std::string_view value);

// The default endpoint when --server is absent: <data dir>/server.sock. Matches
// rpc::DefaultSocketPath()/SocketTarget() on the server side.
std::string DefaultServerTarget();

std::string UsageText();

}  // namespace minimal::client
