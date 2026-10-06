#pragma once

// Helpers shared by the memory tools (tools_read.cpp, tools_write.cpp).

#include <cmath>
#include <ctime>
#include <string>

#include <nlohmann/json.hpp>

#include "memory_runtime.hpp"

#include "minimal/tools/tool.hpp"
#include "minimal/common/clock.hpp"
#include "policy.hpp"

namespace minimal::memory::detail {

inline std::string Iso8601(std::int64_t ms) {
    const std::time_t seconds = common::UnixMillisToTimeT(ms);
    std::tm tm{};
    gmtime_r(&seconds, &tm);
    char buffer[sizeof("YYYY-MM-DDTHH:MM:SSZ")];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buffer;
}

// A score as shown to the model (kScoreScale).
inline double RoundScore(double score) { return std::round(score * kScoreScale) / kScoreScale; }

inline const char* ScopeName(int scope) { return scope == kScopeGlobal ? "global" : "project"; }

// Model-supplied text reaches JSON output; invalid UTF-8 is replaced, never
// thrown on.
inline std::string Dump(const nlohmann::json& j) {
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

inline tools::ToolResult Ok(const nlohmann::json& body) { return {false, Dump(body)}; }
inline tools::ToolResult Error(std::string message) { return {true, std::move(message)}; }

// Summary shape: what search results and the store response carry.
inline nlohmann::json SummaryJson(const EntryRow& row) {
    nlohmann::json j;
    j["id"] = row.id;
    j["title"] = row.title;
    j["description"] = row.description;
    if (!row.tags.empty()) j["tags"] = row.tags;
    j["scope"] = ScopeName(row.scope);
    if (row.pinned) j["pinned"] = true;
    if (row.group_id) j["source"] = row.origin_ref;
    j["updated_at"] = Iso8601(row.updated_ms);
    return j;
}

// Full shape: memory_get.
inline nlohmann::json FullJson(const EntryRow& row) {
    nlohmann::json j = SummaryJson(row);
    j["body"] = row.body;
    j["origin"] = row.origin;
    j["created_at"] = Iso8601(row.created_ms);
    if (row.group_id) {
        j["group"] = {{"id", *row.group_id},
                      {"chunk", row.chunk_index + 1},
                      {"of", row.chunk_count}};
    }
    return j;
}

}  // namespace minimal::memory::detail
