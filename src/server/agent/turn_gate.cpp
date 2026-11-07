#include "minimal/agent/turn_gate.hpp"

#include <algorithm>
#include <exception>
#include <utility>

namespace minimal::agent {

TurnGateRegistry::TurnGateRegistry(common::Logger& logger) : logger_(logger) {}

void TurnGateRegistry::SetAttribution(std::string plugin_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    attribution_ = std::move(plugin_name);
}

void TurnGateRegistry::ClearAttribution() {
    std::lock_guard<std::mutex> lock(mutex_);
    attribution_.clear();
}

void TurnGateRegistry::Register(std::shared_ptr<TurnGate> gate) {
    if (gate == nullptr) return;
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.push_back({attribution_.empty() ? "core" : attribution_, std::move(gate)});
}

void TurnGateRegistry::RemoveByAttribution(const std::string& plugin_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::erase_if(entries_, [&](const Entry& entry) { return entry.plugin == plugin_name; });
}

std::size_t TurnGateRegistry::CountOwnedBy(const std::string& plugin_name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<std::size_t>(std::count_if(
        entries_.begin(), entries_.end(),
        [&](const Entry& entry) { return entry.plugin == plugin_name; }));
}

std::optional<std::string> TurnGateRegistry::Check(const TurnGateRequest& request) const {
    // Snapshot under the lock, call outside it: a gate is plugin code.
    std::vector<Entry> entries;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        entries = entries_;
    }
    for (const auto& entry : entries) {
        try {
            if (auto rejection = entry.gate->Check(request)) return rejection;
        } catch (const std::exception& error) {
            logger_.warn("session {}: turn gate from '{}' threw; rejecting the turn: {}",
                         request.session_id, entry.plugin, error.what());
            return "turn gate '" + entry.plugin + "' failed: " + error.what();
        } catch (...) {
            logger_.warn("session {}: turn gate from '{}' threw; rejecting the turn",
                         request.session_id, entry.plugin);
            return "turn gate '" + entry.plugin + "' failed";
        }
    }
    return std::nullopt;
}

}  // namespace minimal::agent
