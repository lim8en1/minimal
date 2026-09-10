#pragma once

// The checklist-view plugin's pure data layer: the wire shape of
// "checklist.rpc" payloads, decoded into plain structs, plus the small
// formatters both UI surfaces read. Header-only and host-free (no ClientApi,
// no threads) so tests compile it in directly, the cost-view formatters'
// pattern.
//
// The paired server plugin is plugins/server/checklist:
//   "get"  {"session_id"} -> {"session_id":..., "items":[{id,text,status}]}
//   "list" {}             -> {"checklists":[{session_id, items:[...]}, ...]}

#include <cstdint>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace minimal::checklist_view {

enum class Status { Pending, InProgress, Completed };

struct Item {
    std::int64_t id = 0;
    std::string text;
    Status status = Status::Pending;
};

inline Status ParseStatus(const std::string& name) {
    if (name == "completed") return Status::Completed;
    if (name == "in_progress") return Status::InProgress;
    return Status::Pending;
}

// nullopt: the payload is not well-formed enough to trust at all (not an
// object, no "items" array, a non-string text) -- the caller leaves its cache
// alone, the same treatment as a transport failure. A well-formed but EMPTY
// items array is a different answer -- the checklist tool's `op=set,
// items: []` is documented as "clear the list" -- so it yields an empty
// vector, not nullopt: unlike cost totals (monotonic, so "no value" and
// "back to zero" never diverge), a checklist can legitimately go from
// populated to empty, and conflating that with "could not reach the server"
// left a stale non-empty count on screen for the rest of the session once
// the model cleared its list.
inline std::optional<std::vector<Item>> ParseItems(const nlohmann::json& body) {
    if (!body.is_object()) return std::nullopt;
    const auto items = body.find("items");
    if (items == body.end() || !items->is_array()) return std::nullopt;
    std::vector<Item> out;
    out.reserve(items->size());
    for (const auto& entry : *items) {
        if (!entry.is_object() || !entry.contains("text") || !entry.at("text").is_string()) {
            return std::nullopt;
        }
        Item item;
        item.text = entry.at("text").get<std::string>();
        if (entry.contains("id") && entry.at("id").is_number_integer()) {
            item.id = entry.at("id").get<std::int64_t>();
        }
        if (entry.contains("status") && entry.at("status").is_string()) {
            item.status = ParseStatus(entry.at("status").get<std::string>());
        }
        out.push_back(std::move(item));
    }
    return out;
}

struct Counts {
    int done = 0;
    int total = 0;
    int active = 0;
};

inline Counts CountOf(const std::vector<Item>& items) {
    Counts counts;
    counts.total = static_cast<int>(items.size());
    for (const auto& item : items) {
        if (item.status == Status::Completed) ++counts.done;
        if (item.status == Status::InProgress) ++counts.active;
    }
    return counts;
}

// The {todo} segment's text: "todo done/total", plus an active marker while
// any task is in progress. Empty items -> "" (the unfilled-segment rule: a
// checklist that is empty or missing renders as nothing -- "no tasks" is not
// "0/0").
inline std::string FormatSegmentText(const std::vector<Item>& items) {
    if (items.empty()) return {};
    const Counts counts = CountOf(items);
    std::string text = "todo " + std::to_string(counts.done) + "/" + std::to_string(counts.total);
    if (counts.active > 0) text += " (" + std::to_string(counts.active) + " active)";
    return text;
}

}  // namespace minimal::checklist_view
