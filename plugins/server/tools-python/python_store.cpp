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

std::vector<InterpreterStore::Listing> InterpreterStore::List(
    const std::string& session_id) const {
    std::vector<Listing> out;
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& [id, interpreter] : interpreters_) {
        if (!session_id.empty() && id != session_id) continue;
        Listing row;
        row.session_id = id;
        row.pid = interpreter->pid();
        row.alive = interpreter->alive();
        row.python = interpreter->config().python_path;
        row.version = interpreter->version();
        row.declared_count = interpreter->Declared().size();
        row.venved = !interpreter->config().venv.empty();
        row.pending = interpreter->PendingIds();
        out.push_back(std::move(row));
    }
    return out;
}

}  // namespace minimal::tools_python
