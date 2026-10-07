#pragma once

// Host-side (not SDK): the state behind the AttentionSink plugins get.

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace minimal::client::plugins {

// The host-side state behind AttentionSink, shared by the sink and AppState.
// Created before plugin load; AppState reads it per frame. Thread-safe: Raise
// arrives from plugin code (often a plugin's data thread), reads happen on the
// render thread.
class AttentionState {
public:
    void RaiseTab(const std::string& session_id);
    void ClearTab(const std::string& session_id);

    // A plugin raise wins until cleared; core derivation ORs in underneath.
    bool TabRaised(const std::string& session_id) const;
    std::vector<std::string> RaisedTabs() const;

private:
    mutable std::mutex mtx_;
    std::map<std::string, bool> tab_;
};

}  // namespace minimal::client::plugins
