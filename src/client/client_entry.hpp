#pragma once

// Private to the `minimal` executable: what its three entry points share --
// main (app.cpp), the headless -p / --list-sessions paths (headless.cpp) and
// the TUI (tui_app.cpp). app.cpp used to be all three in one 2,300-line file.

#include <memory>
#include <string>

#include "minimal/client/cli_args.hpp"
#include "minimal/client/session_client.hpp"

namespace minimal::client::app {

// Exit codes are part of the interface: a script piping `minimal -p` needs to
// distinguish "the model finished" from "it was cut off" from "the call failed".
enum ExitCode {
    kOk = 0,
    kTruncated = 2,   // max_tokens / stop_sequence: output is incomplete
    kModelError = 3,  // an ErrorEvent reached us
    kCancelled = 4,
    kPaused = 5,      // the session was paused mid-turn (unreachable from -p,
                      // which never sends SetPausedEvent -- mapped so a future
                      // caller cannot mistake suspension for success)
    kUsage = 64,
    kUnavailable = 69,  // no server, or it refused the session
    kInternalError = 70,  // EX_SOFTWARE: an exception escaped somewhere it shouldn't have
};

// The common case for OpenTab: a brand-new session rooted at the cwd, no
// resume, no explicit preset/name. Callers that need those (the startup
// resume path) build CreateOptions by hand instead.
SessionClient::CreateOptions DefaultCreateOptions();

// Every entry point's connect: auto-spawn (if allowed) BEFORE any client
// channel exists -- the probe channel EnsureServerRunning creates holds the
// process's only sockets at that moment, so a forked server inherits nothing
// to hold open -- then dial and wait up to 5 s. Null after printing the one
// shared "cannot reach" diagnostic.
std::shared_ptr<SessionClient> ConnectOrReport(const std::string& target, bool autospawn);

int RunHeadless(const HeadlessOptions& options);
int RunListSessions(const HeadlessOptions& options);
int RunTui(const ParsedArgs& args);

}  // namespace minimal::client::app
