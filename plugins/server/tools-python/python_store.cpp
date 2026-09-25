#include "python_store.hpp"

namespace minimal::tools_python {

std::shared_ptr<Interpreter> InterpreterStore::Find(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = interpreters_.find(session_id);
    return it == interpreters_.end() ? nullptr : it->second;
}

void InterpreterStore::Add(const std::string& session_id, std::shared_ptr<Interpreter> interpreter) {
    std::shared_ptr<Interpreter> previous;
    {
        std::lock_guard<std::mutex> lock(mu_);
        const auto it = interpreters_.find(session_id);
        if (it != interpreters_.end()) previous = std::move(it->second);
        interpreters_[session_id] = interpreter;
    }
    // Killed OUTSIDE the store lock: HardKill reaps (blocks briefly on a
    // SIGKILL-pending pid) and takes the interpreter's own mutex -- neither
    // belongs under the registry lock.
    if (previous) previous->HardKill("replaced by a fresh interpreter");
}

void InterpreterStore::Erase(const std::string& session_id, const char* reason) {
    std::shared_ptr<Interpreter> interpreter;
    {
        std::lock_guard<std::mutex> lock(mu_);
        const auto it = interpreters_.find(session_id);
        if (it == interpreters_.end()) return;
        interpreter = std::move(it->second);
        interpreters_.erase(it);
    }
    interpreter->HardKill(reason);
}

std::vector<std::shared_ptr<Interpreter>> InterpreterStore::Snapshot(
    const std::string& session_id, std::vector<std::string>* ids) const {
    std::vector<std::shared_ptr<Interpreter>> out;
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& [id, interpreter] : interpreters_) {
        if (!session_id.empty() && id != session_id) continue;
        out.push_back(interpreter);
        if (ids != nullptr) ids->push_back(id);
    }
    return out;
}

std::vector<std::string> InterpreterStore::DeclaredInProject(
    const std::filesystem::path& project_root, const std::string& except_session) const {
    std::vector<std::string> ids;
    const auto interpreters = Snapshot({}, &ids);
    std::vector<std::string> out;
    for (std::size_t i = 0; i < interpreters.size(); ++i) {
        const auto& interpreter = interpreters[i];
        if (ids[i] == except_session || !interpreter->alive()) continue;
        if (interpreter->config().venv.empty() || interpreter->config().cwd != project_root) {
            continue;
        }
        const auto declared = interpreter->Declared();
        out.insert(out.end(), declared.begin(), declared.end());
    }
    return out;
}

std::vector<InterpreterStore::Listing> InterpreterStore::List(
    const std::string& session_id) const {
    // Snapshot first, query after: the interpreter's own mutex is not taken
    // under the registry lock (the same rule Add/Erase follow for HardKill).
    std::vector<std::string> ids;
    const auto interpreters = Snapshot(session_id, &ids);
    std::vector<Listing> out;
    for (std::size_t i = 0; i < interpreters.size(); ++i) {
        const auto& interpreter = interpreters[i];
        Listing row;
        row.session_id = ids[i];
        row.pid = interpreter->pid();
        row.alive = interpreter->alive();
        row.python = interpreter->config().python_path;
        row.version = interpreter->version();
        row.declared_count = interpreter->Declared().size();
        row.venved = !interpreter->config().venv.empty();
        row.pending = interpreter->PendingRows();
        out.push_back(std::move(row));
    }
    return out;
}

}  // namespace minimal::tools_python
