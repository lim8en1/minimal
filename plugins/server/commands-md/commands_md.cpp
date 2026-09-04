#include "commands_md.hpp"

#include <algorithm>
#include <cerrno>
#include <set>
#include <csignal>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <sys/wait.h>
#include <unistd.h>

#include <poll.h>

#include "minimal/commands/command_context.hpp"
#include "minimal/commands/confirmation_broker.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/config/frontmatter.hpp"

namespace minimal::commands_md {

namespace {

// --- process runner -----------------------------------------------------------
//
// One child, two pipes, one deadline. stdout is the value; stderr is the error
// narrative (read concurrently with stdout through poll, so a chatty stderr can
// never deadlock the run). On timeout the child is SIGKILLed and reaped. Output
// is capped at read time: once the cap is crossed the child is killed and the
// overrun reported -- the caller decides whether that is a question for the
// operator or a hard failure.

struct RunOutcome {
    bool ok = false;
    std::string output;       // stdout (empty when exceeded)
    std::string error;        // reason when !ok, stderr tail included
    bool exceeded_cap = false;
};

std::string Tail(std::string text, std::size_t max) {
    if (text.size() <= max) return text;
    return "..." + text.substr(text.size() - max);
}

RunOutcome RunProcess(const std::vector<std::string>& argv, const std::string& shell_script,
                      int timeout_seconds, std::size_t cap_bytes) {
    RunOutcome outcome;

    int out_pipe[2];
    int err_pipe[2];
    if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
        outcome.error = "pipe() failed";
        return outcome;
    }

    const pid_t pid = fork();
    if (pid < 0) {
        outcome.error = "fork() failed";
        return outcome;
    }
    if (pid == 0) {
        // Child. New process group, so a timeout kill takes the whole tree.
        setpgid(0, 0);
        close(out_pipe[0]);
        close(err_pipe[0]);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(out_pipe[1]);
        close(err_pipe[1]);
        if (!shell_script.empty()) {
            execl("/bin/sh", "sh", "-c", shell_script.c_str(), static_cast<char*>(nullptr));
        } else {
            std::vector<char*> child_argv;
            child_argv.reserve(argv.size() + 1);
            for (const auto& arg : argv) child_argv.push_back(const_cast<char*>(arg.c_str()));
            child_argv.push_back(nullptr);
            execvp(child_argv[0], child_argv.data());
        }
        _exit(127);  // exec failed; the parent reports through the exit status
    }

    close(out_pipe[1]);
    close(err_pipe[1]);

    // Deadline: poll timeout in ms; 0 s timeout means wait forever.
    const auto start = std::chrono::steady_clock::now();
    auto remaining_ms = [start, timeout_seconds]() -> int {
        if (timeout_seconds <= 0) return -1;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        const auto budget = std::chrono::milliseconds(timeout_seconds) * 1000;
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(budget) - elapsed;
        return left.count() > 0 ? static_cast<int>(left.count()) : 0;
    };

    std::string out_buf;
    std::string err_buf;
    bool out_open = true;
    bool err_open = true;
    bool timed_out = false;
    bool killed_for_cap = false;

    while (out_open || err_open) {
        pollfd fds[2];
        int count = 0;
        int out_index = -1;
        int err_index = -1;
        if (out_open) {
            out_index = count;
            fds[count++] = {out_pipe[0], POLLIN, 0};
        }
        if (err_open) {
            err_index = count;
            fds[count++] = {err_pipe[0], POLLIN, 0};
        }
        const int ready = poll(fds, static_cast<nfds_t>(count), remaining_ms());
        if (ready < 0) {
            if (errno == EINTR) continue;
            outcome.error = "poll() failed";
            break;
        }
        if (ready == 0) {
            timed_out = true;
            break;
        }
        char chunk[4096];
        if (out_index >= 0 && (fds[out_index].revents & (POLLIN | POLLHUP)) != 0) {
            const ssize_t got = read(out_pipe[0], chunk, sizeof(chunk));
            if (got <= 0) {
                out_open = false;
            } else {
                out_buf.append(chunk, static_cast<std::size_t>(got));
                if (cap_bytes > 0 && out_buf.size() > cap_bytes) {
                    killed_for_cap = true;
                    break;
                }
            }
        }
        if (err_index >= 0 && (fds[err_index].revents & (POLLIN | POLLHUP)) != 0) {
            const ssize_t got = read(err_pipe[0], chunk, sizeof(chunk));
            if (got <= 0) {
                err_open = false;
            } else {
                err_buf.append(chunk, static_cast<std::size_t>(got));
                if (err_buf.size() > (1u << 20)) err_buf.resize(1u << 20);  // bounded narrative
            }
        }
    }

    if (timed_out || killed_for_cap) {
        kill(-pid, SIGKILL);
    }
    close(out_pipe[0]);
    close(err_pipe[0]);

    int status = 0;
    waitpid(pid, &status, 0);

    if (timed_out) {
        outcome.error = "timed out after " + std::to_string(timeout_seconds) + "s";
        outcome.error += !err_buf.empty() ? "; stderr: " + Tail(err_buf, 2000) : "";
        return outcome;
    }
    if (killed_for_cap) {
        outcome.exceeded_cap = true;
        return outcome;
    }
    if (out_open) close(out_pipe[0]);  // reached only via the poll-error break

    if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
        outcome.error = "could not execute: " + (argv.empty() ? shell_script : argv.front());
        return outcome;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        outcome.error = "exit status " +
                        (WIFEXITED(status) ? std::to_string(WEXITSTATUS(status))
                                           : std::string("signal ") + std::to_string(WTERMSIG(status)));
        outcome.error += !err_buf.empty() ? "; stderr: " + Tail(err_buf, 2000) : "";
        return outcome;
    }

    outcome.ok = true;
    outcome.output = std::move(out_buf);
    (void)err_buf;
    return outcome;
}

// --- template rendering --------------------------------------------------------

// Single pass, values inserted verbatim, never re-scanned. Unknown placeholders
// are errors: a silent empty would submit a prompt missing exactly the piece
// the author meant to include.
std::string RenderTemplate(const std::string& body,
                           const std::map<std::string, std::string>& values) {
    std::string out;
    out.reserve(body.size());
    std::size_t i = 0;
    while (i < body.size()) {
        if (body[i] != '{') {
            out += body[i++];
            continue;
        }
        const auto close = body.find('}', i);
        if (close == std::string::npos) {
            out += body[i++];
            continue;
        }
        const std::string token = body.substr(i + 1, close - i - 1);
        const auto it = values.find(token);
        if (it == values.end()) {
            throw std::runtime_error("unknown placeholder {" + token + "} in command body");
        }
        out += it->second;
        i = close + 1;
    }
    return out;
}

// Substitutes {name} into one string (an argv entry or a shell script).
// LENIENT by design: only KNOWN names substitute; anything else in braces
// (JSON payloads, awk programs, shell parameter expansion) passes through
// verbatim. Returns false when the string names an unfilled optional -- the
// caller omits the whole entry, which is what "optional" means on a command
// line.
bool SubstituteEntry(const std::string& entry, const std::map<std::string, std::string>& values,
                     const std::set<std::string>& omittable, std::string& out) {
    std::size_t i = 0;
    while (i < entry.size()) {
        if (entry[i] != '{') {
            out += entry[i++];
            continue;
        }
        const auto close = entry.find('}', i);
        if (close == std::string::npos) {
            out += entry[i++];
            continue;
        }
        const std::string token = entry.substr(i + 1, close - i - 1);
        const auto it = values.find(token);
        if (it == values.end()) {
            // Unknown token: literal, not a mistake. The BODY render is where a
            // typo must fail loudly; a source argument is more likely shell/JSON
            // syntax than a mistyped parameter name.
            out += entry.substr(i, close - i + 1);
            i = close + 1;
            continue;
        }
        if (omittable.count(token) != 0 && it->second.empty()) {
            return false;  // unfilled optional: the whole entry is omitted
        }
        out += it->second;
        i = close + 1;
    }
    return true;
}

std::string JsonValueToText(const nlohmann::json& value) {
    if (value.is_string()) return value.get<std::string>();
    return value.dump();
}

}  // namespace

// --- the command ---------------------------------------------------------------

namespace {

class MdCommand : public commands::PromptShorthandCommand {
    using CommandResult = commands::CommandResult;
public:
    explicit MdCommand(MdCommandSpec spec) : spec_(std::move(spec)) {}

    std::string_view Name() const override { return spec_.name; }
    std::string_view Summary() const override { return spec_.summary; }
    std::string_view Usage() const override {
        usage_ = "/" + spec_.name;
        for (const auto& arg : spec_.args) usage_ += " <" + arg + ">";
        for (const auto& arg : spec_.optional) usage_ += " [" + arg + "]";
        return usage_;
    }

    // Submits a turn by definition: never read-only, never a view.
    CommandResult Execute(commands::CommandContext& context,
                          const commands::CommandArgs& args) override {
        // --- bind ---------------------------------------------------------------
        // Unfilled optional names for THIS invocation (command objects are
        // shared across sessions; per-invocation state stays local).
        std::set<std::string> unfilled;
        // args[x] -> positional x+1; optional fills after the required ones; the
        // opencode desugared names ({args}, {1}..) come along for free.
        std::map<std::string, std::string> values;
        std::string joined;
        for (std::size_t i = 0; i < args.positional.size(); ++i) {
            if (i > 0) joined += " ";
            joined += args.positional[i];
            values[std::to_string(i + 1)] = args.positional[i];
        }
        values["args"] = joined;

        for (std::size_t i = 0; i < spec_.args.size(); ++i) {
            if (i >= args.positional.size()) {
                return CommandResult::Error("missing argument <" + spec_.args[i] + ">: usage " +
                                            std::string(Usage()));
            }
            values[spec_.args[i]] = args.positional[i];
        }
        for (std::size_t i = 0; i < spec_.optional.size(); ++i) {
            const std::size_t at = spec_.args.size() + i;
            // An unfilled optional binds as EMPTY for the template (rendering
            // nothing) and is recorded unfilled for the argv rule below (its
            // entry is omitted, not emptied).
            if (at < args.positional.size()) {
                values[spec_.optional[i]] = args.positional[at];
            } else {
                values[spec_.optional[i]] = "";
                unfilled.insert(spec_.optional[i]);
            }
        }

        // --- evaluate sources, in declaration order ------------------------------
        try {
            for (const auto& source : spec_.sources) {
                std::vector<std::string> argv;
                std::string script;
                if (source.use_shell) {
                    // A shell string cannot omit pieces; an unfilled optional in
                    // a script is an error rather than a silent gap.
                    const bool substituted =
                        SubstituteEntry(source.script, values, unfilled, script);
                    if (!substituted) {
                        return CommandResult::Error(
                            "source '" + source.name +
                            "': an optional argument it references was not provided");
                    }
                } else {
                    for (const auto& entry : source.argv) {
                        std::string piece;
                        // allow_omit: an entry naming an unfilled optional is
                        // OMITTED -- that is what "optional" means on a command
                        // line. An entry naming something unknown is an error.
                        if (SubstituteEntry(entry, values, unfilled, piece)) {
                            argv.push_back(std::move(piece));
                        }
                    }
                    if (argv.empty()) {
                        return CommandResult::Error("source '" + source.name +
                                                    "': every argv entry was omitted");
                    }
                }

                const std::size_t cap_bytes = source.cap_tokens * 4;
                RunOutcome run =
                    RunProcess(argv, script, source.timeout_seconds, cap_bytes);
                if (run.exceeded_cap) {
                    const std::size_t estimated = cap_bytes / 4;
                    if (source.on_exceed == CapAction::Fail) {
                        return CommandResult::Error(
                            "source '" + source.name + "' output exceeded the cap (~" +
                            std::to_string(estimated) + " tokens); nothing was submitted");
                    }
                    // Ask the operator. Over the cap there is no honest default:
                    // proceeding costs real tokens, aborting costs a keystroke.
                    commands::ConfirmationRequest request;
                    request.kind = "command_confirmation";
                    request.title = "output cap exceeded";
                    request.detail = "Source '" + source.name + "' produced more than ~" +
                                     std::to_string(estimated) +
                                     " tokens. Submit the composed prompt anyway?";
                    request.options = {"submit", "cancel"};
                    const auto answer = context.confirmations().RequestConfirmation(request);
                    if (answer.chosen_option != "submit") {
                        return CommandResult::Error("output cap confirmed nothing; nothing was "
                                                    "submitted");
                    }
                    // Re-run without the cap: the process was killed at the cap,
                    // so its output must be produced whole before submission.
                    run = RunProcess(argv, script, source.timeout_seconds, 0);
                    if (!run.ok) {
                        return CommandResult::Error("source '" + source.name + "': " + run.error);
                    }
                }
                if (!run.ok) {
                    return CommandResult::Error("source '" + source.name + "': " + run.error);
                }

                if (source.output == SourceOutput::kJson) {
                    nlohmann::json parsed;
                    try {
                        parsed = nlohmann::json::parse(run.output);
                    } catch (const std::exception& error) {
                        return CommandResult::Error("source '" + source.name +
                                                    "' did not emit valid JSON: " + error.what());
                    }
                    for (const auto& field : source.fields) {
                        if (!parsed.contains(field)) {
                            return CommandResult::Error("source '" + source.name +
                                                        "' output is missing field '" + field +
                                                        "'");
                        }
                        values["output:" + source.name + "." + field] =
                            JsonValueToText(parsed.at(field));
                    }
                }
                // Trailing newlines are echo/process artifacts, not prompt
                // content: strip them so a source inserted mid-sentence does
                // not break the line.
                while (!run.output.empty() &&
                       (run.output.back() == '\n' || run.output.back() == '\r')) {
                    run.output.pop_back();
                }
                values[source.name] = std::move(run.output);
            }

            // --- compose ---------------------------------------------------------
            std::string prompt;
            if (spec_.body.empty()) {
                // Whole-stdout mode: the sole text source's output IS the prompt.
                const Source* text_source = nullptr;
                for (const auto& source : spec_.sources) {
                    if (source.output == SourceOutput::kText) {
                        if (text_source != nullptr) {
                            return CommandResult::Error(
                                "no body and more than one text source; a body is required");
                        }
                        text_source = &source;
                    }
                }
                if (text_source == nullptr) {
                    return CommandResult::Error("no body and no text source to take the prompt "
                                                "from");
                }
                prompt = values.at(text_source->name);
            } else {
                prompt = RenderTemplate(spec_.body, values);
            }

            if (prompt.find_first_not_of(" \t\r\n") == std::string::npos) {
                return CommandResult::Error("the composed prompt is empty; nothing was submitted");
            }

            // The ONE sanctioned path: the composed text becomes the turn itself,
            // appended to history and answered like any user turn (§4.4).
            if (!context.session().SubmitTurn(prompt)) {
                return CommandResult::Error("could not submit the turn (session busy?)");
            }
            return CommandResult::Ok("submitted '" + spec_.name + "' (" +
                                     std::to_string(prompt.size()) + " bytes)");
        } catch (const std::exception& error) {
            // Render/bind failures land here too: an error status line reaches
            // the operator, never the chat, never the model.
            return CommandResult::Error(std::string("command '") + spec_.name +
                                        "' failed: " + error.what());
        }
    }

private:
    MdCommandSpec spec_;
    mutable std::string usage_;
};

}  // namespace

std::unique_ptr<commands::Command> MakeCommand(MdCommandSpec spec) {
    return std::make_unique<MdCommand>(std::move(spec));
}

}  // namespace minimal::commands_md
