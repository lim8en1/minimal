#pragma once

// The plugin's persistent cost history, over PluginStorage -- the shared
// namespaced KV store, never a private file format ("a plugin must call
// these, never reimplement" is the same rule that put path_confinement on the
// ABI surface).
//
// SHAPE. One blob per session, key "sessions/<session_id>", value JSON:
//   {"parent_session_id": "...", "turns": [{"model", "at", "in", "out",
//    "cr", "cw", "reported", "fs", "ls"}, ...]}
// "fs"/"ls" are the turn's transcript seq bounds; blobs written before they
// existed lack them and decode with the -1 sentinel (never re-cut on rewind).
// One blob (not one row per turn) because the only readers are /cost and the
// .rpc service, both of which want the whole session anyway, and the write
// path is turn-granularity (seconds apart), so read-modify-write under
// PluginStorage's per-call mutex is free.
//
// LIFETIME. Blobs survive session termination and are deliberately NOT purged
// on OnSessionEnded -- the checklist precedent: a resumed session keeps its
// history, and there is no session-deleted signal to hook. Growth is one
// small row per session that ever ran a turn; DeletePrefix exists if a
// cleanup command is ever wanted.
//
// DEGRADE, NOT CRASH. storage may be null (a host without the seam): every
// method still compiles, AppendTurn becomes a no-op, Load reports empty, and
// available() lets callers answer with a clear message instead of fake data.
//
// CORRUPTION. A blob that fails to decode throws -- the checklist convention.
// Silent loss of a cost history is worse than an error the command surfaces;
// the observer catches, logs, and moves on, so a corrupt session never wedges
// a turn.

#include <optional>
#include <string>
#include <vector>

#include "cost_model.hpp"
#include "minimal/plugins/plugin_storage.hpp"

namespace minimal::cost_tracker {

struct SessionHistory {
    std::string parent_session_id;
    std::vector<TurnRecord> turns;
};

class CostStore {
public:
    // Borrowed; may be null. Process lifetime either way (never dlclosed).
    explicit CostStore(plugins::PluginStorage* storage);

    bool available() const { return storage_ != nullptr; }

    // Throws std::runtime_error on I/O failure or a corrupt stored blob.
    void AppendTurn(const std::string& session_id, const std::string& parent_session_id,
                    const TurnRecord& turn);

    // Empty turns (and empty parent) when the session has none recorded.
    SessionHistory Load(const std::string& session_id) const;

    // Overwrites the session's blob wholesale -- the OnSessionRewound rollback
    // path. Same throw-on-failure contract as AppendTurn.
    void Save(const std::string& session_id, const SessionHistory& history);

    // Every session id with a recorded turn, sorted.
    std::vector<std::string> SessionIds() const;

    // One snapshot per rate source, under FetchedRatesKey(source_name): separate
    // blobs so each refreshes (and corrupts) independently.
    std::optional<FetchedRates> GetFetchedRates(const std::string& source_name) const;
    // Throws std::runtime_error on I/O failure.
    void SetFetchedRates(const std::string& source_name, const FetchedRates& rates);
    // Deletes every snapshot whose source is not in `configured` -- a source
    // removed or renamed in rate_sources -- and returns their names, sorted.
    std::vector<std::string> PruneFetchedRates(const std::vector<std::string>& configured);

private:
    plugins::PluginStorage* storage_;  // borrowed; may be null
};

}  // namespace minimal::cost_tracker
