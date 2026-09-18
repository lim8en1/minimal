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
    const bool trailing_backslash = !command.empty() && command.back() == '\\';
    return command + (trailing_backslash ? "\n\n" : "\n") +
           "__minimal_rc=$?\n{ pwd -P; printf '\\0'; env -0; } >&" +
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

LaunchedProcess LaunchProcess(const std::string& wrapped, const SessionShellState& state,
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
    // RLIMIT_CPU is a backstop for CPU-bound runaways; the wall-clock deadline
    // normally fires first. The grace keeps the two from racing on a command
    // that legitimately spends its budget.
    spec.cpu_seconds = timeout_ms / 1000 + 5;
    spec.file_size_bytes = settings.max_file_size_bytes;

    auto launched = minimal::sidecar::Launch(spec);
    LaunchedProcess proc;
    proc.ok = launched.ok;
    proc.spawn_error = std::move(launched.spawn_error);
    proc.pid = launched.pid;
    proc.warnings = std::move(launched.warnings);
    if (launched.ok && launched.parent_fds.size() == 2) {
        proc.out_fd = launched.parent_fds[0].release();
        proc.state_fd = launched.parent_fds[1].release();
    }
    return proc;
}

}  // namespace minimal::tools_bash
