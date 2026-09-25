#include "mcp_validation.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <filesystem>
#include <sstream>

#include "minimal/common/config_paths.hpp"

namespace minimal::mcp {
namespace {

// Resolvable the way a spawned process would find it: a bare name is searched
// against PATH, anything containing a slash is used as-is. POSIX access(2)
// rather than std::filesystem::access (C++20, absent from this libc++).
bool CommandResolves(const std::string& command) {
    if (command.empty()) return false;
    const auto executable = [](const std::filesystem::path& path) {
        std::error_code ec;
        // `is_regular_file` writes ec; testing it BEFORE the call tested a
    // default-constructed value, so `!ec` was unconditionally true and the code
    // the call reported was never read. Written correctly at bash_tool.cpp:439
    // and again below in this same file -- three copies, one inert.
    const bool regular = std::filesystem::is_regular_file(path, ec);
    // faccessat with AT_EACCESS, not access(2): access tests the REAL uid,
    // but the server's actual authority is its EFFECTIVE uid (a setuid-gid
    // deployment or a privileged helper would pass access and still fail
    // execve -- or vice versa).
    return regular && !ec &&
           ::faccessat(AT_FDCWD, path.c_str(), X_OK, AT_EACCESS) == 0;
    };
    if (command.find('/') != std::string::npos) {
        return executable(command);
    }
    const auto path_env = common::LookupEnv("PATH");
    if (!path_env) return false;
    std::stringstream streams(*path_env);
    std::string dir;
    while (std::getline(streams, dir, ':')) {
        if (dir.empty()) continue;
        if (executable(std::filesystem::path(dir) / command)) return true;
    }
    return false;
}

bool UrlLooksWellFormed(const std::string& url) {
    const auto sep = url.find("://");
    if (sep == std::string::npos || sep == 0) return false;
    const auto scheme = url.substr(0, sep);
    if (scheme != "http" && scheme != "https") return false;
    const auto rest = url.substr(sep + 3);
    const auto host_end = rest.find_first_of("/?#");
    return !rest.substr(0, host_end).empty();
}

}  // namespace

std::vector<std::string> ValidateMcpConfig(const config::McpsConfig& servers,
                                           const config::ChannelsConfig& channels) {
    std::vector<std::string> out;
    for (const auto& error : servers.errors) out.push_back("mcps.json: " + error);
    for (const auto& error : channels.errors) out.push_back("channels.json: " + error);
    for (const auto& entry : servers.entries) {
        if (!entry.enabled) continue;  // a disabled entry is not going to spawn
        if (entry.transport == config::McpTransport::Stdio) {
            if (!CommandResolves(entry.command)) {
                out.push_back("mcps.json entry '" + entry.name + "': command '" + entry.command +
                              "' does not resolve on PATH");
            }
        } else if (!UrlLooksWellFormed(entry.url)) {
            out.push_back("mcps.json entry '" + entry.name + "': url '" + entry.url +
                          "' is not a well-formed http(s) url");
        }
    }
    for (const auto& entry : channels.entries) {
        if (!entry.enabled) continue;
        if (!CommandResolves(entry.command)) {
            out.push_back("channel '" + entry.name + "': command '" + entry.command +
                          "' does not resolve on PATH");
        }
        // The internal source is not addressable as a config channel: an
        // entry with this name would be unreachable from subscriptions AND
        // would collide with cross-session routing, so the manager skips it.
        // Named here so the user is not left guessing why it never fires.
        if (entry.name == config::kReservedInternalChannelName) {
            out.push_back("channel '" + entry.name +
                          "' is reserved for internal cross-session signals; the channel will "
                          "not be configured");
        }
        if (servers.Find(entry.name) != nullptr) {
            out.push_back("channel '" + entry.name +
                          "' is also an mcps.json server name; the channel is skipped and the "
                          "MCP server keeps the name");
        }
    }
    return out;
}

}  // namespace minimal::mcp
