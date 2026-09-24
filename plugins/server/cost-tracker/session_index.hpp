#pragma once

// In-memory child -> parent map over the cost history, so the totals path
// can walk ONE session's subagent subtree (or its ancestor chain) without
// loading every stored blob per call. The totals RPC used to do exactly
// that -- SessionIds() + Load() of every session ever recorded -- on every
// status-line poll, a cost that grew without bound with history.
//
// BUILT LAZILY, ONCE: the first query loads every blob a single time to
// learn the parent edges (a corrupt blob is skipped and logged, the same
// rule the totals path always followed). After that the observer keeps it
// current through Note() -- it sees info.parent_session_id on every round
// and turn, and it is the store's only writer, so no edge can appear that
// the index does not hear about. A parent is pinned by the first write (the
// store's own rule), so an edge never changes once known.
//
// THREADING: one mutex; Note() is called on the observer thread, queries on
// gRPC handler threads and the push hub's worker.

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "cost_store.hpp"

namespace minimal::cost_tracker {

class SessionIndex {
public:
    // Borrowed; process lifetime.
    explicit SessionIndex(const CostStore& store) : store_(store) {}

    // Records the edge learned live. An empty parent marks a root. Cheap
    // before the lazy build (the build will learn the same edge from the
    // store, or keep this one if the blob is not written yet).
    void Note(const std::string& session_id, const std::string& parent_session_id);

    // `root` first, then its descendants breadth-first. Visited-guarded: a
    // corrupt blob forging a parent cycle still terminates.
    std::vector<std::string> Subtree(const std::string& root);

    // `session_id`'s parent, grandparent, ... (not `session_id` itself).
    // Cycle-guarded like Subtree.
    std::vector<std::string> Ancestors(const std::string& session_id);

private:
    void EnsureBuiltLocked();
    void AddEdgeLocked(const std::string& session_id, const std::string& parent_session_id);

    const CostStore& store_;
    std::mutex mtx_;
    bool built_ = false;
    std::map<std::string, std::string> parent_;
    std::map<std::string, std::vector<std::string>> children_;
};

}  // namespace minimal::cost_tracker
