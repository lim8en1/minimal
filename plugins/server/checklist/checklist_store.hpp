#pragma once

// The checklist data model: per-session item lists over PluginStorage, plus the
// JSON codec and the markdown rendering. Pure logic -- no Tool, no RPC -- so
// the tests exercise it directly.
//
// STATE MODEL. One checklist per session, keyed by the session id in this
// plugin's storage namespace. It survives session termination (a resumed
// session keeps its list) and is deliberately NOT purged on session end: rows
// are tiny, there is no session-deleted signal to hook, and purging on
// OnSessionEnded would break the resume case. Growth is one small row per
// session that ever used the tool -- accepted, flagged in ARCHITECTURE.md.
//
// DEGRADE, NOT CRASH. storage may be null (a host without the seam): Load
// returns empty and Save reports failure, so callers answer with a clear error
// instead of throwing.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "minimal/plugins/plugin_storage.hpp"

namespace minimal::checklist {

inline constexpr char kPluginName[] = "checklist";
// The PluginStorage namespace every row of this plugin lives under.
inline constexpr char kStorageNamespace[] = "checklist";

enum class ItemStatus { Pending, InProgress, Completed };

std::optional<ItemStatus> ParseStatus(std::string_view name);
std::string_view StatusName(ItemStatus status);

struct Item {
    std::int64_t id = 0;
    std::string text;
    ItemStatus status = ItemStatus::Pending;
};

class ChecklistStore {
public:
    // Borrowed; may be null. Process lifetime either way (never dlclosed).
    explicit ChecklistStore(plugins::PluginStorage* storage);

    // False when the host did not offer the storage seam: callers degrade with
    // a clear message instead of reporting an empty list as real state.
    bool available() const { return storage_ != nullptr; }

    // The session's items in stored order; empty when it has none.
    std::vector<Item> Load(const std::string& session_id) const;

    // Replaces the session's list. Throws std::runtime_error on I/O failure.
    void Save(const std::string& session_id, const std::vector<Item>& items);

    // Every session id with a stored checklist, sorted. The `list` RPC's
    // enumeration path.
    std::vector<std::string> SessionIds() const;

private:
    plugins::PluginStorage* storage_;
};

// Strict decode of the stored JSON ({"items":[{id,text,status}]}). Throws
// std::runtime_error naming the problem on anything malformed -- a row this
// plugin cannot parse is corruption or a newer schema, and a silent empty list
// would look exactly like lost data.
std::vector<Item> DecodeItems(std::string_view json);
std::string EncodeItems(const std::vector<Item>& items);

// The wire form of a list -- [{id,text,status}] -- shared by the .rpc replies
// and the push stream, so the two can never disagree on shape.
nlohmann::json ItemsJson(const std::vector<Item>& items);

// GFM tasklist rendering -- `1. [x]` / `2. [ ]`, numbered by item id in stored
// order, with the done/total count in the footer. This is the MODEL's copy of
// the list (the tool result text); UI rendering is a client's business and
// gets the same items through the endpoint.
std::string RenderMarkdown(const std::vector<Item>& items);

}  // namespace minimal::checklist
