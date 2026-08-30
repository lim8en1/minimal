#include "bash_session_state.hpp"

#include <cstring>
#include <unistd.h>

// POSIX declares it in <unistd.h> only under feature-test macros that C++ mode
// does not guarantee; declare it ourselves.
extern char** environ;

namespace minimal::tools_bash {
namespace {

// The server process's environment, as a sorted map. `environ` is a POSIX
// global; reading it here (at seed time, under our lock) is the only access.
std::map<std::string, std::string> ProcessEnvironment() {
    std::map<std::string, std::string> env;
    if (environ == nullptr) return env;
    for (char** entry = environ; *entry != nullptr; ++entry) {
        const char* equals = std::strchr(*entry, '=');
        if (equals == nullptr || equals == *entry) continue;  // malformed / empty key
        env.emplace(std::string(*entry, equals - *entry), equals + 1);
    }
    return env;
}

}  // namespace

SessionShellState BashSessionStateStore::Seed(const std::filesystem::path& project_root) {
    SessionShellState state;
    // Canonical so `pwd` and cd-then-`..` agree with confine()'s canonicalized
    // results (on macOS /var is a symlink to /private/var). A missing root falls
    // back to the raw path: the sanity check owns that failure, not the seed.
    std::error_code ec;
    const auto canonical = std::filesystem::canonical(project_root, ec);
    state.cwd = ec ? project_root : canonical;
    state.env = ProcessEnvironment();
    return state;
}

SessionShellState BashSessionStateStore::Snapshot(const std::string& session_id,
                                                  const std::filesystem::path& project_root) {
    std::lock_guard lock(mutex_);
    auto it = states_.find(session_id);
    if (it == states_.end()) {
        it = states_.emplace(session_id, Seed(project_root)).first;
    }
    return it->second;
}

void BashSessionStateStore::SetState(const std::string& session_id, std::filesystem::path cwd,
                                     std::map<std::string, std::string> env) {
    std::lock_guard lock(mutex_);
    auto it = states_.find(session_id);
    if (it == states_.end()) return;
    it->second.cwd = std::move(cwd);
    it->second.env = std::move(env);
}

void BashSessionStateStore::Reset(const std::string& session_id,
                                  const std::filesystem::path& project_root) {
    std::lock_guard lock(mutex_);
    states_[session_id] = Seed(project_root);
}

void BashSessionStateStore::Erase(const std::string& session_id) {
    std::lock_guard lock(mutex_);
    states_.erase(session_id);
}

bool BashSessionStateStore::Has(const std::string& session_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return states_.find(session_id) != states_.end();
}

std::size_t BashSessionStateStore::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return states_.size();
}

}  // namespace minimal::tools_bash
