#include "minimal/client/cli_args.hpp"

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <system_error>

#include "minimal/common/config_paths.hpp"

namespace minimal::client {
namespace {

// Flag-shaped: starts with '-' and is not the conventional bare "-" (stdin).
// This is the test that stops `-p --server ...` from taking "--server" as a
// prompt. It costs the ability to write a dash-leading value positionally, which
// is what the --flag=value form is for.
bool LooksLikeFlag(std::string_view token) { return token.size() > 1 && token.front() == '-'; }

std::string ExpandTilde(std::string_view path) {
    if (path.empty() || path.front() != '~') return std::string(path);
    if (path.size() > 1 && path[1] != '/') return std::string(path);  // ~user is not handled
    const char* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') return std::string(path);
    return std::string(home) + std::string(path.substr(1));
}

}  // namespace

std::string ResolveServerTarget(std::string_view value) {
    if (value.empty()) return DefaultServerTarget();
    // Already a target: unix://, dns:///, ipv4:, unix-abstract:, ...
    if (value.find("://") != std::string_view::npos) return std::string(value);
    if (value.find(':') != std::string_view::npos && value.front() != '/' &&
        value.front() != '.' && value.front() != '~') {
        return std::string(value);  // host:port
    }

    const std::string expanded = ExpandTilde(value);
    const bool path_shaped = expanded.front() == '/' || expanded.rfind("./", 0) == 0 ||
                             expanded.rfind("../", 0) == 0 ||
                             (expanded.size() >= 5 && expanded.compare(expanded.size() - 5, 5,
                                                                       ".sock") == 0);
    if (!path_shaped) return expanded;

    // Relative paths are resolved here rather than handed to gRPC, which would
    // interpret them against its own notion of the cwd rather than the user's.
    std::error_code ec;
    auto absolute = std::filesystem::absolute(expanded, ec);
    return "unix://" + (ec ? expanded : absolute.lexically_normal().string());
}

std::string DefaultServerTarget() {
    return "unix://" + (common::DataDir() / "server.sock").string();
}

std::string UsageText() {
    return "usage: minimal [-p PROMPT] [options]\n"
           "\n"
           "  -p, --print PROMPT        run one turn headlessly and exit\n"
           "      --resume SESSION_ID   continue a stored session (see ListSessions);\n"
           "                              it keeps its stored preset and project_root\n"
           "      --server TARGET       socket path or gRPC target\n"
           "                              (default: unix://<data dir>/server.sock;\n"
           "                               a bare path pairs with minimal-server --listen)\n"
           "      --agent PRESET        agent preset (default: settings.json)\n"
           "      --output-format FMT   text (default) or jsonl\n"
           "      --dangerously-bypass  skip every permission check\n"
           "      --version\n"
           "\n"
           "Values that start with '-' must use the --flag=value form, e.g.\n"
           "  minimal --print=--version-is-not-a-flag-here\n";
}

ParsedArgs ParseArgs(int argc, const char* const* argv) {
    ParsedArgs parsed;
    bool headless = false;
    bool prompt_set = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];

        // Split --flag=value once, so every value-taking flag below gets the
        // same treatment and the inline form is not a special case per flag.
        std::string_view name = arg;
        std::string_view inline_value;
        bool has_inline = false;
        if (arg.rfind("--", 0) == 0) {
            if (const auto eq = arg.find('='); eq != std::string_view::npos) {
                name = arg.substr(0, eq);
                inline_value = arg.substr(eq + 1);
                has_inline = true;
            }
        }

        // Returns the flag's value, or leaves `parsed` in the error state and
        // returns nullopt. Rejecting a flag-shaped value is the whole point:
        // consuming it is how `-p --server X "prompt"` sent "--server" to the
        // model and dropped the other two arguments without a word.
        auto take_value = [&](std::string_view flag) -> std::optional<std::string_view> {
            if (has_inline) return inline_value;
            if (i + 1 >= argc) {
                parsed.action = ParsedArgs::Action::Error;
                parsed.error = std::string(flag) + " needs a value";
                return std::nullopt;
            }
            const std::string_view value = argv[i + 1];
            if (LooksLikeFlag(value)) {
                parsed.action = ParsedArgs::Action::Error;
                parsed.error = std::string(flag) + " needs a value, but the next argument is "
                                                   "another flag: " +
                               std::string(value) + "\n" + "if that really is the value, write it "
                                                           "as " +
                               std::string(flag) + "=" + std::string(value);
                return std::nullopt;
            }
            ++i;
            return value;
        };

        if (name == "--version") {
            parsed.action = ParsedArgs::Action::Version;
            return parsed;
        }
        if (name == "--help" || name == "-h") {
            parsed.action = ParsedArgs::Action::Usage;
            return parsed;
        }

        if (name == "-p" || name == "--print") {
            const auto value = take_value(name);
            if (!value) return parsed;
            headless = true;
            prompt_set = true;
            parsed.headless.prompt = *value;
        } else if (name == "--server") {
            const auto value = take_value(name);
            if (!value) return parsed;
            parsed.headless.server = ResolveServerTarget(*value);
        } else if (name == "--resume") {
            const auto value = take_value(name);
            if (!value) return parsed;
            if (value->empty()) {
                parsed.action = ParsedArgs::Action::Error;
                parsed.error = "--resume needs a session id";
                return parsed;
            }
            parsed.headless.resume_session_id = *value;
        } else if (name == "--agent") {
            const auto value = take_value(name);
            if (!value) return parsed;
            parsed.headless.agent_preset = *value;
        } else if (name == "--output-format") {
            const auto value = take_value(name);
            if (!value) return parsed;
            if (*value == "jsonl") {
                parsed.headless.jsonl = true;
            } else if (*value != "text") {
                parsed.action = ParsedArgs::Action::Error;
                parsed.error = "unknown --output-format '" + std::string(*value) +
                               "' (want text or jsonl)";
                return parsed;
            }
        } else if (name == "--dangerously-bypass") {
            parsed.headless.bypass = true;
        } else {
            // NOT IGNORED. Dropping unrecognised arguments is what let the -p bug
            // above stay quiet: the swallowed target and the real prompt both
            // landed here and vanished.
            parsed.action = ParsedArgs::Action::Error;
            parsed.error = "unexpected argument: " + std::string(arg);
            if (!LooksLikeFlag(arg)) {
                parsed.error += "\nvalues belong to a flag -- did you mean --server=" +
                                std::string(arg) + " or -p " + std::string(arg) + "?";
            }
            return parsed;
        }
    }

    if (headless && prompt_set && parsed.headless.prompt.empty()) {
        parsed.action = ParsedArgs::Action::Error;
        parsed.error = "-p needs a non-empty prompt";
        return parsed;
    }
    if (!headless && !parsed.headless.resume_session_id.empty()) {
        parsed.action = ParsedArgs::Action::Error;
        parsed.error = "--resume needs -p: resuming runs one headless turn";
        return parsed;
    }
    if (!parsed.headless.resume_session_id.empty() && !parsed.headless.agent_preset.empty()) {
        parsed.action = ParsedArgs::Action::Error;
        parsed.error = "--resume and --agent cannot be combined: a resumed session keeps the "
                       "preset it was stored with";
        return parsed;
    }
    parsed.action = headless ? ParsedArgs::Action::Headless : ParsedArgs::Action::Tui;
    return parsed;
}

}  // namespace minimal::client
