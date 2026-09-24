#pragma once

// The TUI's /-command catalog, PULLED fresh per tab scope (it used to be one
// list fetched at connect, for the startup project only).
//
// Why pull: the server's project commands change under it (an edited or added
// <project>/commands/*.md is picked up by ProjectCommandSource's fingerprint
// check), and tabs can live in different projects. Every read goes through
// Get(scope), which answers IMMEDIATELY with the last known list for that
// scope (stale-while-revalidate) and, at most once per `min_refresh`, starts a
// background ListCommands for it. The consumers -- the prompt editor's
// /-autocomplete (per frame while a command name is typed), the /help
// overlay (per open), the sessionless unknown-command check -- all call Get,
// so opening autocomplete IS the refresh trigger; nothing polls when nobody
// is looking.
//
// A scope is the focused tab's session id once it has one (the server
// resolves that session's own root), else the project root its first message
// would create the session with (AppState::FocusedCommandScope). A scope
// with no list of its own yet falls back to the connect-time seed.
//
// Thread model: Get/Listed/Seed run on the loop thread; the fetch completion
// runs on the RPC worker. Snapshots are shared_ptr<const> and replaced, never
// mutated, so a snapshot a frame is iterating stays valid across a swap. The
// completion holds only a weak reference, so a fetch landing after the cache
// is gone is dropped.

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "minimal/client/view/prompt_editor.hpp"

namespace minimal::client {

class CommandCatalogCache {
public:
    using Entries = std::vector<CommandCatalogEntry>;
    using Snapshot = std::shared_ptr<const Entries>;
    using Clock = std::chrono::steady_clock;
    // Starts a ListCommands for (session_id | project_root) and calls `done`
    // exactly once with the server's entries, or nullopt on failure. May
    // call it on any thread, including synchronously.
    using Fetch = std::function<void(const std::string& session_id,
                                     const std::string& project_root,
                                     std::function<void(std::optional<Entries>)> done)>;

    // `client_side`: entries the client intercepts itself (/new, /theme, ...)
    // and appends to every server list.
    CommandCatalogCache(Entries client_side, Fetch fetch,
                        std::chrono::milliseconds min_refresh = std::chrono::seconds(2),
                        std::function<Clock::time_point()> now = &Clock::now);

    // Records an already-fetched server list for a scope (the synchronous
    // connect-time fetch) and makes it the fallback for scopes not yet
    // fetched. A later Get for the same scope still refreshes on schedule.
    void Seed(const std::string& session_id, const std::string& project_root, Entries server);

    // Never null. See the header comment.
    Snapshot Get(const std::string& session_id, const std::string& project_root);

    // Whether Get's answer for this scope reflects a real server list (its
    // own or the seed's) -- false means client-side entries only, so a name
    // missing from it proves nothing.
    bool Listed(const std::string& session_id, const std::string& project_root);

private:
    struct Slot {
        Snapshot snapshot;  // null until this scope's first fetch lands
        bool in_flight = false;
        bool started = false;
        Clock::time_point last_start{};
    };
    struct State {
        std::mutex mutex;
        std::map<std::string, Slot> slots;  // by ScopeKey
        Snapshot seed;                      // null until Seed
        Snapshot client_only;
        Entries client_side;
    };
    static std::string ScopeKey(const std::string& session_id, const std::string& project_root);
    Snapshot MergeLocked(Entries server) const;

    std::shared_ptr<State> state_;
    Fetch fetch_;
    std::chrono::milliseconds min_refresh_;
    std::function<Clock::time_point()> now_;
};

}  // namespace minimal::client
