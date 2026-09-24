#include "minimal/client/command_catalog.hpp"

#include <utility>

namespace minimal::client {

CommandCatalogCache::CommandCatalogCache(Entries client_side, Fetch fetch,
                                         std::chrono::milliseconds min_refresh,
                                         std::function<Clock::time_point()> now)
    : state_(std::make_shared<State>()),
      fetch_(std::move(fetch)),
      min_refresh_(min_refresh),
      now_(std::move(now)) {
    state_->client_only = std::make_shared<const Entries>(client_side);
    state_->client_side = std::move(client_side);
}

std::string CommandCatalogCache::ScopeKey(const std::string& session_id,
                                          const std::string& project_root) {
    return session_id.empty() ? "r:" + project_root : "s:" + session_id;
}

CommandCatalogCache::Snapshot CommandCatalogCache::MergeLocked(Entries server) const {
    server.insert(server.end(), state_->client_side.begin(), state_->client_side.end());
    return std::make_shared<const Entries>(std::move(server));
}

void CommandCatalogCache::Seed(const std::string& session_id, const std::string& project_root,
                               Entries server) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    Snapshot merged = MergeLocked(std::move(server));
    Slot& slot = state_->slots[ScopeKey(session_id, project_root)];
    slot.snapshot = merged;
    slot.started = true;
    slot.last_start = now_();
    state_->seed = std::move(merged);
}

CommandCatalogCache::Snapshot CommandCatalogCache::Get(const std::string& session_id,
                                                       const std::string& project_root) {
    const std::string key = ScopeKey(session_id, project_root);
    Snapshot answer;
    bool start = false;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        Slot& slot = state_->slots[key];
        const auto now = now_();
        if (fetch_ && !slot.in_flight &&
            (!slot.started || now - slot.last_start >= min_refresh_)) {
            slot.in_flight = true;
            slot.started = true;
            slot.last_start = now;
            start = true;
        }
        answer = slot.snapshot ? slot.snapshot
                 : state_->seed ? state_->seed
                                : state_->client_only;
    }
    // Outside the lock: a Fetch may complete synchronously.
    if (start) {
        std::weak_ptr<State> weak = state_;
        fetch_(session_id, project_root, [weak, key](std::optional<Entries> server) {
            const auto state = weak.lock();
            if (!state) return;   // the cache is gone; nothing to publish into
            std::lock_guard<std::mutex> lock(state->mutex);
            Slot& slot = state->slots[key];
            slot.in_flight = false;
            // A failure keeps the last known list; the next Get past
            // min_refresh retries.
            if (!server) return;
            server->insert(server->end(), state->client_side.begin(), state->client_side.end());
            slot.snapshot = std::make_shared<const Entries>(std::move(*server));
        });
    }
    return answer;
}

bool CommandCatalogCache::Listed(const std::string& session_id, const std::string& project_root) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto it = state_->slots.find(ScopeKey(session_id, project_root));
    return (it != state_->slots.end() && it->second.snapshot) || state_->seed != nullptr;
}

}  // namespace minimal::client
