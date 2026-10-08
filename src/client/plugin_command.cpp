#include "minimal/client/plugin_command.hpp"

#include <spawn.h>
#include <sys/wait.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <optional>

#include <nlohmann/json.hpp>

#include "minimal/client/cli_args.hpp"
#include "minimal/client/server_spawn.hpp"
#include "minimal/client/session_client.hpp"
#include "minimal/common/plugin_config_label.hpp"

extern char** environ;

namespace minimal::client {
namespace {

// Runs `argv` with this process's terminal; its exit code, or 1 when it could
// not be started or did not exit normally.
int RunChild(const std::vector<std::string>& argv) {
    std::vector<char*> raw;
    raw.reserve(argv.size() + 1);
    for (const auto& arg : argv) raw.push_back(const_cast<char*>(arg.c_str()));
    raw.push_back(nullptr);

    pid_t pid = 0;
    if (const int error = ::posix_spawn(&pid, raw[0], nullptr, nullptr, raw.data(), environ);
        error != 0) {
        std::fprintf(stderr, "minimal: could not run %s: %s\n", raw[0], std::strerror(error));
        return 1;
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return 1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

// The running server's `plugins` endpoint answer to `method`, or nullopt
// when no server is running (one is never spawned for this) or it could not
// be reached -- the caller then edits the file itself.
std::optional<nlohmann::json> CallRunningServer(const std::string& method,
                                                const nlohmann::json& request) {
    const std::string target = DefaultServerTarget();
    SpawnOptions no_spawn;
    no_spawn.enabled = false;
    if (!EnsureServerRunning(target, no_spawn)) return std::nullopt;
    SessionClient client(target);
    if (!client.WaitForConnected(std::chrono::seconds(2))) return std::nullopt;
    // A server of another build may not even have the method, or may apply
    // it differently: edit the file, which the next server start reads.
    const auto info = client.GetServerInfo();
    const auto check = CheckServerBuild(info.status, info.info, target, /*spawned=*/false);
    if (check.verdict == ServerBuildCheck::Verdict::kOther) {
        std::fprintf(stderr,
                     "minimal: %s\nminimal: editing the file directly -- that server is not "
                     "told; restart it to apply\n",
                     check.message.c_str());
        return std::nullopt;
    }
    const auto result = client.PluginCall("plugins", method, request.dump());
    // Every fallback says why, and that the running server was not told.
    if (!result.status.ok()) {
        std::fprintf(stderr,
                     "minimal: the running server did not take the edit (%s) -- editing the file "
                     "directly; restart the server to apply it\n",
                     result.status.error_message().c_str());
        return std::nullopt;
    }
    auto answer = nlohmann::json::parse(result.json_payload, nullptr, /*allow_exceptions=*/false);
    if (!answer.is_object()) {
        std::fprintf(stderr,
                     "minimal: the running server answered something unreadable -- editing the "
                     "file directly; restart the server to apply it\n");
        return std::nullopt;
    }
    return answer;
}

// `set <name> <key> <value>` / `unset <name> <key>` through a running
// server: validated by the plugin inside the server's own process and
// environment (an "env:" key reference resolves against the server's
// variables, not this shell's), written, and applied. nullopt when there is
// no server to ask, or it does not know the plugin (one installed since it
// started): the local CLI handles those.
std::optional<int> EditThroughServer(const std::vector<std::string>& args) {
    const bool set = args.size() == 4 && args[0] == "set";
    const bool unset = args.size() == 3 && args[0] == "unset";
    if (!set && !unset) return std::nullopt;
    nlohmann::json request{{"name", args[1]}, {"key", args[2]}};
    if (set) request["text"] = args[3];
    const auto answer = CallRunningServer(args[0], request);
    if (!answer) return std::nullopt;
    if (answer->contains("error")) {
        const std::string error = answer->value("error", std::string());
        if (error.rfind("no plugin '", 0) == 0) {
            // A disabled-at-start plugin is still listed; this one the
            // running server never saw (installed since it started).
            std::fprintf(stderr,
                         "minimal: the running server has no plugin '%s' (installed since it "
                         "started?) -- editing the file directly; it applies at the next start\n",
                         args[1].c_str());
            return std::nullopt;
        }
        std::fprintf(stderr, "minimal: %s\n", error.c_str());
        return 1;
    }
    // The server names the file (a core file is not under plugins.d/).
    const std::string label =
        answer->value("file", common::PluginConfigLabel(common::kPluginConfigDirName, args[1])) +
        ": " + args[2];
    const std::string what = label + (set ? " set" : " back to its default");
    const std::string applied = answer->value("applied", "");
    if (applied == "live") {
        std::printf("%s (applied by the running server, no restart needed)\n", what.c_str());
    } else if (applied == "restart") {
        std::string why;
        if (answer->contains("reconfigure_error")) {
            why = "; the plugin refused it live: " + answer->value("reconfigure_error", std::string());
        }
        std::printf("%s (restart the server to apply%s)\n", what.c_str(), why.c_str());
    } else if (applied == "not_running") {
        std::printf("%s (saved; %s -- it takes effect when it starts)\n", what.c_str(),
                    answer->value("reason", std::string("the plugin is not running")).c_str());
    } else if (const auto unused = answer->value("unused", nlohmann::json::object());
               unused.contains(args[2])) {
        std::printf("%s (saved; not used while %s)\n", what.c_str(),
                    unused.at(args[2]).get<std::string>().c_str());
    } else {
        std::printf("%s (no change)\n", what.c_str());
    }
    return 0;
}

}  // namespace

int RunPluginCommand(const std::vector<std::string>& args) {
    // --json changes nothing about an edit; only the bare verb form is routed.
    std::vector<std::string> bare;
    for (const auto& arg : args) {
        if (arg != "--json") bare.push_back(arg);
    }
    if (const auto code = EditThroughServer(bare)) return *code;

    const std::string server = LocateServerBinary();
    if (server.empty()) {
        std::fprintf(stderr,
                     "minimal: cannot find minimal-server (set MINIMAL_SERVER_BIN to its path)\n");
        return 1;
    }
    std::vector<std::string> argv = {server, "plugin"};
    argv.insert(argv.end(), args.begin(), args.end());
    return RunChild(argv);
}

}  // namespace minimal::client
