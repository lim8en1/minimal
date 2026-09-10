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

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace minimal::client {

// A `/name args` prompt, split. Present only when the prompt was one.
struct CommandInvocationText {
    std::string name;
    std::vector<std::string> positional;
    std::string raw;
};

struct HeadlessOptions {
    std::string prompt;
    std::string server;  // already a gRPC target; empty means the default socket
    std::string agent_preset;
    // "<provider>/<model>", validated at parse so a typo fails before a server
    // is contacted. Empty means the preset's own model.
    std::string model;
    // Set when the prompt was a /-command rather than text.
    std::optional<CommandInvocationText> command;
    // Non-empty means resume this stored session instead of creating a new one.
    // Mutually exclusive with agent_preset: the stored session keeps its preset.
    std::string resume_session_id;
    // Signal subscriptions for the session, comma-separated selectors
    // ("ci/build.completed", "reminder", "sessions"). Validated server-side at
    // session start; works on resume too (subscriptions never persist).
    std::vector<std::string> signals;
    bool jsonl = false;
    // --output-format json: a machine-readable listing, valid only alongside
    // --list-sessions (a turn's output formats stay text|jsonl).
    bool list_json = false;
    // --list-sessions: list every project's sessions, not just the cwd's.
    bool all_dirs = false;
    bool bypass = false;
    // When nothing is listening on a unix-socket target, the client starts one
    // (see server_spawn.hpp). --no-autospawn restores the old "cannot reach"
    // error; a non-unix target never spawns regardless.
    bool autospawn = true;
};

struct ParsedArgs {
    enum class Action { Tui, Headless, ListSessions, Usage, Version, Error };

    Action action = Action::Tui;
    HeadlessOptions headless;
    // Set when action == Error. Already formatted for stderr, possibly several
    // lines: the second line is usually the fix, which is the part that was
    // missing when this silently did the wrong thing.
    std::string error;
};

ParsedArgs ParseArgs(int argc, const char* const* argv);

// Splits "/name arg1 arg2" into its parts. Client-side because this is the side
// holding the text a human typed; the wire carries the result, so the server
// never re-splits a command line. The prompt editor's `/` path uses the same
// split the headless flag does -- one parser, not two opinions.
CommandInvocationText SplitCommand(std::string_view text);

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
