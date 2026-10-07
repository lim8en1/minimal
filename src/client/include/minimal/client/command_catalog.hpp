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

#include "minimal/client/sdk/command_catalog_entry.hpp"


namespace minimal::client {

// True when the command is usable on a tab with NO session: either the
// server marks it requires_session=false, it submits a prompt (a PromptShorthand
// such as a markdown /review: the host creates the session first, exactly as a
// first chat message does, then runs the command on it), or it is one of the two
// desired-state commands (/agent, /model) whose bare form opens a
// client-owned picker and whose named form sets the tab's desired state --
// neither ever refuses for want of a session (app.cpp's submit_command
// intercepts both before any dispatch). ONE rule gates the /-autocomplete,
// the /help overlay, and the sessionless refusal, so a command offered on a
// sessionless tab is never one that would only refuse there. The names are
// necessarily hardcoded: the interception that makes them sessionless-safe
// is client-side by design (the server's taxonomy is per-command, and these
// two are per-invocation).
inline bool UsableWithoutSession(const CommandCatalogEntry& entry) {
    return !entry.requires_session || entry.submits_prompt || entry.name == "agent" || entry.name == "model";
}

// True when the command is usable on a SUBAGENT tab. A child's command
// surface is READ-ONLY server-side (SessionServiceImpl::DispatchCommand
// refuses everything else), so the palette offers only what would not be
// refused there: read-only catalog entries (/cost, /sessions, ...), plus the
// two client-intercepted commands that work on a child (/new opens a tab of
// its own; /detach-session closes the focused child). /agent and /model are
// deliberately NOT usable: their bare forms open pickers and their named
// forms stage desired state, all aimed at the focused tab's session -- and
// choosing what a CHILD runs on is the parent preset's business, not the
// observer's. ONE rule gates the /-autocomplete and the /help overlay, the
// same pattern as UsableWithoutSession above.
inline bool UsableOnSubagent(const CommandCatalogEntry& entry) {
    return entry.read_only || entry.name == "new" || entry.name == "detach-session";
}

// Returns a SHARED SNAPSHOT, never null: the catalog is refreshed in the
// background (CommandCatalogCache) and replaced wholesale, so a caller keeps
// the one it got valid for as long as it holds it, and the pointer's identity
// tells a caller whether the catalog changed since it last looked. Invoked on
// every frame while a line still reads like a `/`-command name; a
// shared_ptr copy, not a vector copy.
using CommandCatalogSource =
    std::function<std::shared_ptr<const std::vector<CommandCatalogEntry>>()>;


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

    // Replaces the client's own entries (the ones appended after every
    // server list). Meant for startup, before the first Seed/Get: lists
    // merged under the old entries are dropped and refetch.
    void SetClientSide(Entries client_side);

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
