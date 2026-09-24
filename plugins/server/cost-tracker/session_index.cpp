#include "session_index.hpp"

#include <algorithm>
#include <set>

#include "minimal/common/logger.hpp"

namespace minimal::cost_tracker {

void SessionIndex::Note(const std::string& session_id, const std::string& parent_session_id) {
    if (session_id.empty()) return;
    std::lock_guard<std::mutex> lock(mtx_);
    AddEdgeLocked(session_id, parent_session_id);
}

std::vector<std::string> SessionIndex::Subtree(const std::string& root) {
    std::lock_guard<std::mutex> lock(mtx_);
    EnsureBuiltLocked();
    std::vector<std::string> subtree{root};
    std::set<std::string> visited{root};
    for (std::size_t i = 0; i < subtree.size(); ++i) {
        const auto kids = children_.find(subtree[i]);
        if (kids == children_.end()) continue;
        for (const auto& child : kids->second) {
            if (visited.insert(child).second) subtree.push_back(child);
        }
    }
    return subtree;
}

std::vector<std::string> SessionIndex::Ancestors(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mtx_);
    EnsureBuiltLocked();
    std::vector<std::string> ancestors;
    std::set<std::string> visited{session_id};
    for (auto it = parent_.find(session_id); it != parent_.end() && !it->second.empty();
         it = parent_.find(it->second)) {
        if (!visited.insert(it->second).second) break;
        ancestors.push_back(it->second);
    }
    return ancestors;
}

void SessionIndex::EnsureBuiltLocked() {
    if (built_) return;
    built_ = true;
    for (const auto& id : store_.SessionIds()) {
        try {
            AddEdgeLocked(id, store_.Load(id).parent_session_id);
        } catch (const std::exception& error) {
            // One corrupt blob loses one edge, not the index -- the same skip
            // rule the totals and list paths follow.
            minimal::common::DefaultLogger().warn(
                "cost-tracker: skipping unreadable cost history for session {}: {}", id,
                error.what());
        }
    }
}

void SessionIndex::AddEdgeLocked(const std::string& session_id,
                                 const std::string& parent_session_id) {
    // First write pins the parent (the store's rule): a known edge is final.
    const auto [it, inserted] = parent_.emplace(session_id, parent_session_id);
    if (!inserted) {
        if (!it->second.empty() || parent_session_id.empty()) return;
        it->second = parent_session_id;   // a root-by-default learns its real parent
    }
    if (parent_session_id.empty()) return;
    auto& kids = children_[parent_session_id];
    if (std::find(kids.begin(), kids.end(), session_id) == kids.end()) kids.push_back(session_id);
}

}  // namespace minimal::cost_tracker
