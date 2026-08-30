#pragma once

#include <cstddef>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>

namespace minimal::tools_bash {

// One session's persistent shell state: cwd and environment ONLY. The process
// is per-call (stateless); this map is what makes `cd`/`export` chain across
// calls -- and what makes operator-rejection tolerable, since a denied command
// leaves no half-mutated interpreter behind.
struct SessionShellState {
    std::filesystem::path cwd;
    // Sorted, so the envp handed to exec has deterministic order.
    std::map<std::string, std::string> env;
};

// Plugin-owned and keyed by session id -- core holds no bash state (see the
// plugin's header comment). Without OnSessionEnded this map would grow for the
// process lifetime; Erase() is that hook, called from BashSessionObserver in
// this plugin's plugin.cpp.
class BashSessionStateStore {
public:
    // Existing state, or a fresh one seeded from `project_root` (bash's initial
    // cwd) and the server process's environment (the model expects $HOME & co).
    SessionShellState Snapshot(const std::string& session_id, const std::filesystem::path& project_root);

    // Applied wholesale from what the shell reported on its state channel after a
    // command finished. Wholesale rather than per-variable because `unset` and
    // any `export` inside the command must both be reflected -- a merge would
    // resurrect variables the command deliberately removed.
    void SetState(const std::string& session_id, std::filesystem::path cwd,
                  std::map<std::string, std::string> env);

    // Back to the seed: project_root + a fresh copy of the process environment.
    void Reset(const std::string& session_id, const std::filesystem::path& project_root);

    void Erase(const std::string& session_id);

    // Observable so a test can assert that OnSessionEnded actually shrank the
    // map -- "no longer leaks" is not a claim inspection can settle.
    bool Has(const std::string& session_id) const;
    std::size_t size() const;

private:
    SessionShellState Seed(const std::filesystem::path& project_root);

    mutable std::mutex mutex_;
    std::unordered_map<std::string, SessionShellState> states_;
};

}  // namespace minimal::tools_bash
