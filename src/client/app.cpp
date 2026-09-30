// `minimal` entry point: TUI by default, headless with -p.
//
// The headless mode is not a TUI flag bolted on -- it is a separate client against
// the same SessionService, and the reason PermissionRunMode::AllowOnly exists.
// Building it first is what proves the proto is a real boundary rather than a
// TUI-shaped hole, and it is the natural end-to-end test harness.

// The `minimal` executable's entry point: argument parsing, logging setup, and
// dispatch to the headless paths (headless.cpp) or the TUI (tui_app.cpp).

#include <csignal>
#include <cstdio>
#include <exception>

#include "client_entry.hpp"
#include "minimal/client/cli_args.hpp"
#include "minimal/common/abi_tag.hpp"
#include "minimal/common/config_paths.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/common/logging_setup.hpp"

using namespace minimal::client::app;   // the ExitCode values

int main(int argc, char** argv) {
    // Matches the server's process-wide disposition: the client writes to
    // gRPC sockets and to stdout -- which a piped script may close early --
    // and neither should kill the process by default disposition. stdout
    // writes after that simply fail; the run completes and the exit status
    // goes where it can.
    std::signal(SIGPIPE, SIG_IGN);

    const auto parsed = minimal::client::ParseArgs(argc, argv);
    switch (parsed.action) {
        case minimal::client::ParsedArgs::Action::Version:
            std::printf("minimal %s\n", minimal::common::Version());
            return kOk;
        case minimal::client::ParsedArgs::Action::Usage:
            std::fputs(minimal::client::UsageText().c_str(), stdout);
            return kOk;
        case minimal::client::ParsedArgs::Action::Error:
            std::fprintf(stderr, "%s\n\n", parsed.error.c_str());
            std::fputs(minimal::client::UsageText().c_str(), stderr);
            return kUsage;
        case minimal::client::ParsedArgs::Action::Headless:
        case minimal::client::ParsedArgs::Action::ListSessions:
        case minimal::client::ParsedArgs::Action::Tui:
            break;
    }

    // LOGGING BEFORE ANYTHING ELSE LOGS: DefaultLogger resolves to spdlog's
    // console default on first use, and an out-of-band stdout/stderr write
    // desyncs the alternate screen the same way a resize does (see RunTui);
    // headless stdout carries JSONL. So client logs go to a rotating FILE
    // under the data dir, never the terminal. Configured after the
    // usage/version exits above -- those print to stdout on purpose -- and
    // before any client code runs, per the ordering contract in
    // logging_setup.hpp.
    // A safety net for anything that throws before its own catch can run --
    // DataDir()/ConfigDir() with no $HOME, plugin discovery, BuildTuiEnvironment
    // -- none of which has entered the terminal's raw/alt-screen mode yet, so
    // there is nothing to restore; this only turns a bare SIGABRT (exit 134)
    // into a clean message and exit code. RunTui has its own inner try/catch
    // around screen.Loop() for throws *during* the TUI (see its shutdown_tui).
    try {
        minimal::common::LoggingOptions logging;
        logging.path = (minimal::common::DataDir() / "log" / "client.log").string();
        // File-only: the client owns the terminal, and headless stdout carries
        // JSONL. If the file cannot be opened, ConfigureLogging degrades back to
        // the console sink rather than dropping every message.
        logging.console = false;
        const auto logging_error = minimal::common::ConfigureLogging(logging);
        if (!logging_error.empty()) {
            minimal::common::DefaultLogger().warn("logging: {}", logging_error);
        }

        if (parsed.action == minimal::client::ParsedArgs::Action::Headless) {
            return minimal::client::app::RunHeadless(parsed.headless);
        }
        if (parsed.action == minimal::client::ParsedArgs::Action::ListSessions) {
            return minimal::client::app::RunListSessions(parsed.headless);
        }
        return minimal::client::app::RunTui(parsed);
    } catch (const std::exception& error) {
        minimal::common::DefaultLogger().error("main: escaping exception: {}", error.what());
        std::fprintf(stderr, "minimal: internal error: %s\n", error.what());
        return kInternalError;
    } catch (...) {
        minimal::common::DefaultLogger().error("main: escaping non-standard exception");
        std::fprintf(stderr, "minimal: internal error (non-standard exception)\n");
        return kInternalError;
    }
}
