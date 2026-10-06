#include "bash_process.hpp"

#include <unistd.h>

namespace minimal::tools_bash {

// --- The state trailer -------------------------------------------------------------

std::string WrapCommand(const std::string& command) {
    // A command ending in a backslash is a line continuation: backslash-newline
    // glues the next physical line onto it, so the plain separator below made
    // `ls \` run `ls __minimal_rc=$?` -- the marker became an argument of the
    // user's command, and the state the trailer exists to report was lost. An
    // EXTRA newline first is correct for both parities: an odd trailing
    // backslash count consumes the first newline as continuation and the second
    // ends the (now empty) logical line; an even count is a literal backslash,
    // the first newline ends the command, and the second is a no-op empty line.
    // The absolute path, not a PATH lookup: the command itself may replace
    // PATH (`export PATH=$HOME/bin`), which otherwise fails to resolve `env`
    // (appending a spurious "env: command not found" to the model-visible
    // output and silently dropping the whole state update -- ParseStatePayload
    // sees no env entries and discards the payload wholesale). `command -p
    // env` was tried first and rejected: POSIX leaves it unspecified whether
    // `-p`'s guaranteed PATH is also substituted into the child's OWN
    // environment, and on this shell it is -- `env -0`'s report of PATH
    // itself came back as the fixed default, not the session's real one,
    // which is exactly the state this trailer exists to capture correctly.
    const bool trailing_backslash = !command.empty() && command.back() == '\\';
    return command + (trailing_backslash ? "\n\n" : "\n") +
           "__minimal_rc=$?\n{ pwd -P; printf '\\0'; /usr/bin/env -0; } >&" +
           std::to_string(kStateFd) + "\nexit $__minimal_rc\n";
}

std::string ComposeResult(const std::string& output, const std::string& footer,
                           const std::string& grandchild_note) {
    if (grandchild_note.empty()) {
        return output.empty() ? footer : output + "\n" + footer;
    }
    const std::string tail =
        footer.empty() ? grandchild_note : footer + "\n" + grandchild_note;
    return output.empty() ? tail : output + "\n" + tail;
}

ShellState ParseStatePayload(const std::string& payload) {
    ShellState state;
    const auto nul = payload.find('\0');
    if (nul == std::string::npos || nul == 0) return state;

    // `pwd` terminates its line, so the cwd arrives with a trailing newline that
    // would make every subsequent chdir() fail.
    std::string cwd = payload.substr(0, nul);
    while (!cwd.empty() && (cwd.back() == '\n' || cwd.back() == '\r')) cwd.pop_back();
    if (cwd.empty() || cwd.front() != '/') return state;  // must be an absolute path
    state.cwd = cwd;

    std::size_t pos = nul + 1;
    while (pos < payload.size()) {
        const auto end = payload.find('\0', pos);
        const auto entry = payload.substr(pos, end == std::string::npos ? std::string::npos
                                                                         : end - pos);
        const auto eq = entry.find('=');
        if (eq != std::string::npos && eq != 0) {
            state.env.emplace(entry.substr(0, eq), entry.substr(eq + 1));
        }
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    if (state.env.empty()) return state;  // a shell always reports something
    state.ok = true;
    return state;
}

// --- Launch ------------------------------------------------------------------------

minimal::sidecar::LaunchSpec BuildLaunchSpec(const std::string& wrapped, const SessionShellState& state,
                                             const BashSettings& settings, std::int64_t timeout_ms) {
    // The launch shape, expressed over the sidecar core's generic launcher:
    // pipe 0 is merged stdout+stderr (one write end dup2'd onto both, so the
    // streams interleave in order), pipe 1 is the fd-9 state channel -- a
    // passthrough fd, so its CLOEXEC must be cleared explicitly or exec
    // closes it and every cd/export is silently lost.
    minimal::sidecar::LaunchSpec spec;
    spec.argv = {settings.shell, "-c", wrapped};
    spec.cwd = state.cwd;
    spec.env = state.env;
    spec.pipes = {
        minimal::sidecar::PipePlan{
            minimal::sidecar::PipePlan::ChildEnd::kWrite,
            {{STDOUT_FILENO, false}, {STDERR_FILENO, false}}},
        minimal::sidecar::PipePlan{
            minimal::sidecar::PipePlan::ChildEnd::kWrite, {{kStateFd, true}}},
    };
    spec.cpu_seconds = minimal::sidecar::CpuBackstopSeconds(std::chrono::milliseconds(timeout_ms));
    spec.file_size_bytes = settings.max_file_size_bytes;
    return spec;
}

minimal::sidecar::Launched LaunchProcess(const std::string& wrapped, const SessionShellState& state,
                                         const BashSettings& settings, std::int64_t timeout_ms) {
    return minimal::sidecar::Launch(BuildLaunchSpec(wrapped, state, settings, timeout_ms));
}

}  // namespace minimal::tools_bash
