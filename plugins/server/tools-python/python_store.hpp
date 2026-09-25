#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "python_interpreter.hpp"

namespace minimal::tools_python {

// The plugin's per-session interpreter registry -- the same ownership shape as
// tools-bash's cwd/env map: state keyed by session id lives INSIDE this .so,
// the SessionObserver frees it, and the reader thread (the liveness source of
// truth) handles every kill path. The store itself only owns records.
class InterpreterStore {
public:
    // The live interpreter for this session, or null. The shared_ptr keeps the
    // object (and its reader thread) alive across any concurrent erase.
    std::shared_ptr<Interpreter> Find(const std::string& session_id);

    // Records a freshly spawned interpreter. Any previous entry for the
    // session is killed first -- there is exactly one interpreter per session.
    void Add(const std::string& session_id, std::shared_ptr<Interpreter> interpreter);

    // Drops the record AND kills the process. Idempotent; safe to call from
    // the fire-and-forget OnSessionEnded and from explicit kills alike (the
    // reader thread converges on the same outcome through dead_).
    void Erase(const std::string& session_id, const char* reason);

    // The declared records of every OTHER session's live venv interpreter in
    // this project: what the shared venv must keep satisfying (see
    // ResolveAgainstShared). Call under ProjectInstallMutex so a concurrent
    // spawn cannot slip between this check and its own registration.
    std::vector<std::string> DeclaredInProject(const std::filesystem::path& project_root,
                                               const std::string& except_session) const;

    // Interpreters alive per session, for the /python command.
    struct Listing {
        std::string session_id;
        pid_t pid = -1;
        bool alive = false;
        std::string python;   // interpreter binary
        std::string version;
        std::size_t declared_count = 0;
        bool venved = false;
        std::vector<Interpreter::PendingRow> pending;
    };
    std::vector<Listing> List(const std::string& session_id) const;

private:
    // The interpreters (and their session ids) matching `session_id` (empty =
    // all), copied out under the registry lock.
    std::vector<std::shared_ptr<Interpreter>> Snapshot(const std::string& session_id,
                                                       std::vector<std::string>* ids) const;

    mutable std::mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<Interpreter>> interpreters_;
};

}  // namespace minimal::tools_python
