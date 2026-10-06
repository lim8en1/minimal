#include "minimal/client/plugin_command.hpp"

#include <spawn.h>
#include <sys/wait.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>

#include <nlohmann/json.hpp>

#include "minimal/client/cli_args.hpp"
#include "minimal/client/server_spawn.hpp"
#include "minimal/client/session_client.hpp"

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

// After an edit: a running server (never one spawned for this) re-reads the
// plugin's file and applies what it can live. No server, nothing to say.
void TellRunningServer(const std::string& plugin) {
    const std::string target = DefaultServerTarget();
    SpawnOptions no_spawn;
    no_spawn.enabled = false;
    if (!EnsureServerRunning(target, no_spawn)) return;
    SessionClient client(target);
    if (!client.WaitForConnected(std::chrono::seconds(2))) return;
    const auto result = client.PluginCall("plugins", "apply",
                                          nlohmann::json{{"name", plugin}}.dump());
    if (!result.status.ok()) {
        std::fprintf(stderr, "minimal: running server: %s\n",
                     result.status.error_message().c_str());
        return;
    }
    const auto answer =
        nlohmann::json::parse(result.json_payload, nullptr, /*allow_exceptions=*/false);
    if (!answer.is_object()) return;
    if (answer.contains("error")) {
        std::fprintf(stderr, "minimal: running server: %s\n",
                     answer.at("error").get<std::string>().c_str());
        return;
    }
    const std::string applied = answer.value("applied", "");
    if (applied == "live") {
        std::printf("running server: applied, no restart needed\n");
    } else if (applied == "restart") {
        std::string why;
        if (answer.contains("reconfigure_error")) {
            why = " (the plugin refused it live: " +
                  answer.at("reconfigure_error").get<std::string>() + ")";
        }
        std::printf("running server: restart it to apply%s\n", why.c_str());
    }
}

}  // namespace

int RunPluginCommand(const std::vector<std::string>& args) {
    const std::string server = LocateServerBinary();
    if (server.empty()) {
        std::fprintf(stderr,
                     "minimal: cannot find minimal-server (set MINIMAL_SERVER_BIN to its path)\n");
        return 1;
    }
    std::vector<std::string> argv = {server, "plugin"};
    argv.insert(argv.end(), args.begin(), args.end());
    const int code = RunChild(argv);
    if (code == 0 && args.size() >= 2 && (args[0] == "set" || args[0] == "unset")) {
        TellRunningServer(args[1]);
    }
    return code;
}

}  // namespace minimal::client
