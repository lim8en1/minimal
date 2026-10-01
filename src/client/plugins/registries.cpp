#include "minimal/client/plugins/registries.hpp"

#include <algorithm>
#include <utility>

#include "minimal/common/logger.hpp"

namespace minimal::client::plugins {

// --- AttentionState ----------------------------------------------------------

void AttentionState::RaiseTab(const std::string& session_id) {
    if (session_id.empty()) return;   // "" is every sessionless tab, never one session
    std::lock_guard<std::mutex> lock(mtx_);
    tab_[session_id] = true;
}

void AttentionState::ClearTab(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mtx_);
    tab_.erase(session_id);   // erased, not set false: the map only holds raised tabs
}

bool AttentionState::TabRaised(const std::string& session_id) const {
    if (session_id.empty()) return false;
    std::lock_guard<std::mutex> lock(mtx_);
    const auto it = tab_.find(session_id);
    return it != tab_.end() && it->second;
}

std::vector<std::string> AttentionState::RaisedTabs() const {
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<std::string> out;
    for (const auto& [session_id, raised] : tab_) {
        if (raised) out.push_back(session_id);
    }
    return out;
}

// --- Registry mechanics -------------------------------------------------------

bool RegistryBase::Admit() const {
    if (!sealed_) return true;
    common::DefaultLogger().warn(
        "client plugin '{}': registration ignored -- the registries were sealed after "
        "startup (registration-time-only contract)",
        attribution_.empty() ? "<core>" : attribution_);
    return false;
}

void WarnTaken(const std::string& holder, const std::string& claimant) {
    common::DefaultLogger().warn(
        "client plugin '{}': exclusive slot already claimed by '{}' -- registration ignored "
        "(the exclusive-slot rule: first registration wins)",
        claimant, holder.empty() ? "the core default" : holder);
}

// --- MessageRendererRegistry --------------------------------------------------

void MessageRendererRegistry::Register(ChatRowKind kind, std::unique_ptr<MessageRenderer> renderer) {
    if (!Admit()) return;
    if (renderer == nullptr) return;   // a stored null would Find() as null and silently disable the kind
    attributions_.push_back({attribution_, std::string(ToString(kind))});
    kinds_.push_back(kind);
    // First claimant stays active; the duplicate is visible in attributions()
    // and the loader turns it fatal. Silently replacing would make
    // presentation depend on plugin load order -- exactly what the collision
    // rule exists to prevent.
    if (renderers_.find(kind) == renderers_.end()) {
        renderers_.emplace(kind, std::move(renderer));
    } else {
        common::DefaultLogger().warn(
            "client plugin '{}': message-kind '{}' already claimed -- registration recorded "
            "and ignored (first claimant wins; the duplicate is fatal at startup)",
            attribution_, ToString(kind));
    }
}

const MessageRenderer* MessageRendererRegistry::Find(ChatRowKind kind) const {
    const auto it = renderers_.find(kind);
    return it == renderers_.end() ? nullptr : it->second.get();
}

bool MessageRendererRegistry::PluginClaimed(ChatRowKind kind) const {
    for (std::size_t i = 0; i < kinds_.size(); ++i) {
        if (kinds_[i] == kind) return !attributions_[i].plugin_name.empty();
    }
    return false;
}

void MessageRendererRegistry::RemoveByAttribution(const std::string& plugin_name) {
    // Only the FIRST claim for a kind won the slot (a later duplicate is
    // recorded but never reaches renderers_): erase renderers_[kind] only
    // when THIS plugin is that first claimant, never for a losing duplicate
    // naming a kind an earlier plugin still owns.
    std::map<ChatRowKind, bool> owner_decided;
    for (std::size_t i = 0; i < kinds_.size(); ++i) {
        if (owner_decided[kinds_[i]]) continue;
        owner_decided[kinds_[i]] = true;
        if (attributions_[i].plugin_name == plugin_name) renderers_.erase(kinds_[i]);
    }
    std::size_t write = 0;
    for (std::size_t read = 0; read < attributions_.size(); ++read) {
        if (attributions_[read].plugin_name == plugin_name) continue;
        if (write != read) {
            attributions_[write] = std::move(attributions_[read]);
            kinds_[write] = kinds_[read];
        }
        ++write;
    }
    attributions_.resize(write);
    kinds_.resize(write);
}

}  // namespace minimal::client::plugins
