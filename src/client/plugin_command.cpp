#include "minimal/client/plugin_command.hpp"

#include <spawn.h>
#include <sys/wait.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "minimal/client/server_spawn.hpp"

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
    return RunChild(argv);
}

}  // namespace minimal::client
