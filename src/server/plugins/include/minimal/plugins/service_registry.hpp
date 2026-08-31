#pragma once

// The sanctioned cross-plugin channel: one .so reaching another's SERVICES.
//
// SessionObserver covers ordered notification (push); this covers queries
// (pull). Before it, a plugin owning state simply could not be asked for it --
// which is why /restart-shell lived inside tools-bash and why CommandContext
// documented other plugins' state as deliberately unreachable. Ownership stays
// with the providing plugin: a service is the sanctioned way TO ASK, not a
// state grab. See docs/EXTENSIBILITY.md §2.2.
//
// TYPED AT THE EDGES, NAMED IN THE MIDDLE. Core never sees a type: a plugin
// Provides a void* under a name, and a consumer static_casts Find's result to
// the interface header the PROVIDER ships (e.g. tools-bash's
// bash_state_service.hpp). The name is the only runtime coupling; the header is
// the contract, under the same source-compatibility model as everything else.
//
// The provider's interface must be ABSTRACT (header-only, pure virtual). The
// concrete object's symbols live inside the providing .so, which is loaded
// RTLD_LOCAL -- a consumer calling them by symbol would fail to resolve at
// dlopen(RTLD_NOW). Virtual dispatch needs no symbols: the call goes through
// the object's own vtable, which points back into the provider.
//
// NO BUS, NO SUBSCRIBE. Push already exists (SessionObserver); this is pull,
// deliberately. An event bus would put the ordering/concurrency burden back on
// plugin authors -- exactly what serialized observer dispatch removed.

#include "minimal/common/logger.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace minimal::plugins {

// Transparent hashing so Find can take a string_view without materializing a
// std::string per lookup.
struct TransparentStringHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view s) const noexcept {
        return std::hash<std::string_view>{}(s);
    }
};

class ServiceRegistry {
public:
    // Register an abstract interface under a name. Ownership stays with the
    // providing plugin; the registry holds a non-owning pointer for the process
    // lifetime (plugin handles are never dlclosed -- see plugin_loader.hpp's
    // LIFETIME note). A null service is ignored: registering "nothing" would
    // turn Find's null contract ("absent") into a lie.
    //
    // Registration-time only. Provide is called from plugin load on the main
    // thread; Find is called later, from session workers and the command pool.
    // The map is unguarded by design -- a runtime Provide would race concurrent
    // Finds AND slip past collision detection, which runs once at startup --
    // so after Seal() a Provide is logged and ignored, and the frozen contract
    // is a property of the registry rather than a comment about load order.
    void Provide(std::string name, void* service) {
        if (service == nullptr) return;
        if (sealed_) {
            common::DefaultLogger().warn(
                "service '{}' provided after plugin load completed; ignored -- Provide is a "
                "registration-time operation",
                name);
            return;
        }
        services_[name] = service;
        // Recorded under the open attribution window -- empty for a
        // registration made by core itself, the same convention ToolRegistry
        // uses for built-ins.
        attributions_.push_back({attribution_, name});
    }

    // Named lookup. Null when absent: the provider plugin may be disabled,
    // which is a runtime condition, not an error. Consumers resolve LAZILY at
    // use time, not at registration -- plugin load order is file order, so a
    // consumer can load before its provider.
    void* Find(std::string_view name) const {
        if (auto it = services_.find(name); it != services_.end()) {
            return it->second;
        }
        return nullptr;
    }

    // Called once by main.cpp after all plugins load. From then on the map is
    // read-only, which is what makes unguarded Find safe from any thread.
    void Seal() { sealed_ = true; }

    // --- Attribution, for collision reporting -------------------------------
    //
    // Same pattern as ToolRegistry's: the loader opens a scoped window around
    // each plugin's registration callback so every name arriving inside it is
    // attributed to that plugin, which is what lets a collision name both
    // culprits. Not thread-safe, and not needed to be: plugin loading is
    // single-threaded and completes before any session exists.
    void SetAttribution(std::string plugin_name) { attribution_ = std::move(plugin_name); }
    void ClearAttribution() { attribution_.clear(); }

    struct Attribution {
        std::string plugin_name;   // empty for registrations made by core itself
        std::string service_name;
    };
    const std::vector<Attribution>& attributions() const { return attributions_; }

private:
    std::string attribution_;
    std::vector<Attribution> attributions_;
    bool sealed_ = false;
    std::unordered_map<std::string, void*, TransparentStringHash, std::equal_to<>> services_;
};

}  // namespace minimal::plugins
