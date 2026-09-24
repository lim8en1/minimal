#pragma once

// A kind-keyed registry whose entries remember which plugin registered them.
//
// The shape shared by every "one implementation per kind string" plugin
// registry (provider factories, model/metadata catalogs, variant params,
// compaction strategies): first registration of a kind wins; a later one is
// refused AND recorded as a collision naming both plugins, so the startup
// sanity check can point at the culprits instead of the winner silently
// depending on load order. The loader brackets each plugin's registration
// hook with SetAttribution/ClearAttribution, and can strip everything a
// failed plugin registered with RemoveByAttribution.
//
// Not thread-safe: written only during plugin load, read-only afterwards.

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace minimal::plugins {

template <typename T>
class AttributedRegistry {
public:
    struct Collision {
        std::string kind;
        std::string first_plugin;   // the registration that holds the slot
        std::string second_plugin;  // the one that was dropped
    };

    // False (and a recorded collision) when `kind` is already taken. Callers
    // validate `value` themselves before calling -- what counts as a usable
    // value is the concrete registry's business.
    bool Register(std::string kind, T value) {
        const auto it = entries_.find(kind);
        if (it != entries_.end()) {
            collisions_.push_back(Collision{std::move(kind), it->second.plugin, attribution_});
            return false;
        }
        entries_.emplace(std::move(kind), Entry{std::move(value), attribution_});
        return true;
    }

    const T* FindEntry(std::string_view kind) const {
        const auto it = entries_.find(kind);
        return it == entries_.end() ? nullptr : &it->second.value;
    }

    bool Has(std::string_view kind) const { return entries_.find(kind) != entries_.end(); }

    // Sorted: map iteration order.
    std::vector<std::string> kinds() const {
        std::vector<std::string> out;
        out.reserve(entries_.size());
        for (const auto& [kind, _] : entries_) out.push_back(kind);
        return out;
    }

    void SetAttribution(std::string plugin_name) { attribution_ = std::move(plugin_name); }
    void ClearAttribution() { attribution_.clear(); }
    const std::string& attribution() const { return attribution_; }

    const std::vector<Collision>& collisions() const { return collisions_; }

    // Kinds `plugin_name` holds, sorted. Empty name: core's own registrations.
    std::vector<std::string> KeysOwnedBy(const std::string& plugin_name) const {
        std::vector<std::string> out;
        for (const auto& [kind, entry] : entries_) {
            if (entry.plugin == plugin_name) out.push_back(kind);
        }
        return out;
    }

    void RemoveByAttribution(const std::string& plugin_name) {
        std::erase_if(entries_, [&](const auto& item) { return item.second.plugin == plugin_name; });
    }

private:
    struct Entry {
        T value;
        std::string plugin;
    };
    std::map<std::string, Entry, std::less<>> entries_;
    std::string attribution_;
    std::vector<Collision> collisions_;
};

}  // namespace minimal::plugins
