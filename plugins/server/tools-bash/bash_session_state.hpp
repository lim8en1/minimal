#pragma once

#include <cstddef>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>

#include "bash_state_service.hpp"

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
//
// Implements the shipped BashSessionStateService contract so the store can be
// Provided under kBashStateServiceName; the virtual surface is exactly that
// interface's, which is what keeps the service narrow.
class BashSessionStateStore : public BashSessionStateService {
public:
    // inherit_env: "allowlist" (default) seeds a filtered environment -- the
    // server process's environment carries provider API keys and anything else
    // the operator exported, and a seed of ALL of it let a model-authored
    // `env` dump credentials into the transcript and the persisted store. The
    // allowlist is what the model legitimately needs ($HOME, $PATH, $TMPDIR);
    // "all" restores the old behavior as an explicit operator choice.
    explicit BashSessionStateStore(std::string inherit_env = "allowlist")
        : inherit_env_(std::move(inherit_env)) {}

    // Existing state, or a fresh one seeded from `project_root` (bash's initial
    // cwd) and the (filtered, by default) process environment.
    SessionShellState Snapshot(const std::string& session_id, const std::filesystem::path& project_root);

    // Applied wholesale from what the shell reported on its state channel after a
    // command finished. Wholesale rather than per-variable because `unset` and
    // any `export` inside the command must both be reflected -- a merge would
    // resurrect variables the command deliberately removed.
    void SetState(const std::string& session_id, std::filesystem::path cwd,
                  std::map<std::string, std::string> env);

    // Back to the seed: project_root + a fresh copy of the process environment.
    void Reset(const std::string& session_id,
               const std::filesystem::path& project_root) override;

    void Erase(const std::string& session_id) override;

    // Observable so a test can assert that OnSessionEnded actually shrank the
    // map -- "no longer leaks" is not a claim inspection can settle.
    bool Has(const std::string& session_id) const;
    std::size_t size() const;

private:
    SessionShellState Seed(const std::filesystem::path& project_root);

    std::string inherit_env_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, SessionShellState> states_;
};

}  // namespace minimal::tools_bash
