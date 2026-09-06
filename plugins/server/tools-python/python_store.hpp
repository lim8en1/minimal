#pragma once

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

    // Interpreters alive per session, for the /python command.
    struct Listing {
        std::string session_id;
        pid_t pid;
        bool alive;
        std::string python;   // resolved binary
        std::string version;
        std::size_t declared_count;
        bool venved;
        std::vector<std::string> pending;
    };
    std::vector<Listing> List(const std::string& session_id) const;

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<Interpreter>> interpreters_;
};

}  // namespace minimal::tools_python
