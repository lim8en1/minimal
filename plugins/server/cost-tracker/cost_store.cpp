#include "cost_store.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>

namespace minimal::cost_tracker {
namespace {

constexpr const char* kParentKey = "parent_session_id";
constexpr const char* kTurnsKey = "turns";

constexpr const char* kModelKey = "model";
constexpr const char* kAtKey = "at";
constexpr const char* kInKey = "in";
constexpr const char* kOutKey = "out";
constexpr const char* kCacheReadKey = "cr";
constexpr const char* kCacheWriteKey = "cw";
constexpr const char* kReportedKey = "reported";
constexpr const char* kFirstSeqKey = "fs";
constexpr const char* kLastSeqKey = "ls";

nlohmann::json ParseOrThrow(std::string_view json, const std::string& session_id) {
    try {
        return nlohmann::json::parse(json);
    } catch (const std::exception& error) {
        throw std::runtime_error("cost-tracker: stored history for session " + session_id +
                                 " is not valid JSON: " + error.what());
    }
}

nlohmann::json EncodeTurn(const TurnRecord& turn) {
    return nlohmann::json{
        {kModelKey, turn.model},
        {kAtKey, turn.at_unix_ms},
        {kInKey, turn.usage.input_tokens},
        {kOutKey, turn.usage.output_tokens},
        {kCacheReadKey, turn.usage.cache_read_tokens},
        {kCacheWriteKey, turn.usage.cache_write_tokens},
        {kReportedKey, turn.usage.usage_reported},
        {kFirstSeqKey, turn.first_seq},
        {kLastSeqKey, turn.last_seq},
    };
}

TurnRecord DecodeTurn(const nlohmann::json& entry, const std::string& session_id) {
    if (!entry.is_object()) {
        throw std::runtime_error("cost-tracker: stored turn for session " + session_id +
                                 " is not an object");
    }
    TurnRecord turn;
    if (const auto it = entry.find(kModelKey); it != entry.end() && it->is_string()) {
        turn.model = it->get<std::string>();
    }
    if (const auto it = entry.find(kAtKey); it != entry.end() && it->is_number_integer()) {
        turn.at_unix_ms = it->get<std::int64_t>();
    }
    if (const auto it = entry.find(kInKey); it != entry.end() && it->is_number_integer()) {
        turn.usage.input_tokens = std::max<std::int64_t>(it->get<std::int64_t>(), 0);
    }
    if (const auto it = entry.find(kOutKey); it != entry.end() && it->is_number_integer()) {
        turn.usage.output_tokens = std::max<std::int64_t>(it->get<std::int64_t>(), 0);
    }
    if (const auto it = entry.find(kCacheReadKey); it != entry.end() && it->is_number_integer()) {
        turn.usage.cache_read_tokens = std::max<std::int64_t>(it->get<std::int64_t>(), 0);
    }
    if (const auto it = entry.find(kCacheWriteKey); it != entry.end() && it->is_number_integer()) {
        turn.usage.cache_write_tokens = std::max<std::int64_t>(it->get<std::int64_t>(), 0);
    }
    if (const auto it = entry.find(kReportedKey); it != entry.end() && it->is_boolean()) {
        turn.usage.usage_reported = it->get<bool>();
    }
    // TOLERANT of absence: blobs written before the seq bounds existed decode
    // with the struct's -1 sentinel. A record without bounds is never cut on
    // rewind -- it cannot be classified, and it predates the fix.
    if (const auto it = entry.find(kFirstSeqKey); it != entry.end() && it->is_number_integer()) {
        turn.first_seq = it->get<std::int64_t>();
    }
    if (const auto it = entry.find(kLastSeqKey); it != entry.end() && it->is_number_integer()) {
        turn.last_seq = it->get<std::int64_t>();
    }
    return turn;
}

}  // namespace

CostStore::CostStore(plugins::PluginStorage* storage) : storage_(storage) {}

void CostStore::AppendTurn(const std::string& session_id, const std::string& parent_session_id,
                           const TurnRecord& turn) {
    if (storage_ == nullptr) return;  // degraded: no storage seam, no history
    if (session_id.empty()) {
        // An empty id would store under the degenerate key "session:" --
        // visible to every enumeration and shared by every caller that
        // forgot its id. Same guard checklist_store applies.
        throw std::runtime_error("cost-tracker: no session to record the turn under");
    }
    SessionHistory history;
    if (const auto existing = storage_->Get(kStorageNamespace, kSessionKeyPrefix + session_id)) {
        const nlohmann::json document = ParseOrThrow(*existing, session_id);
        if (!document.is_object()) {
            throw std::runtime_error("cost-tracker: stored history for session " + session_id +
                                     " is not an object");
        }
        if (const auto it = document.find(kParentKey); it != document.end() && it->is_string()) {
            history.parent_session_id = it->get<std::string>();
        }
        if (const auto it = document.find(kTurnsKey); it != document.end() && it->is_array()) {
            for (const auto& entry : *it) history.turns.push_back(DecodeTurn(entry, session_id));
        }
    }
    // The first recorded turn pins the parent: subagent attribution is a
    // property of the session, and SessionInfo repeats it identically on
    // every later OnTurnCompleted anyway.
    if (history.parent_session_id.empty() && !parent_session_id.empty()) {
        history.parent_session_id = parent_session_id;
    }
    history.turns.push_back(turn);
    Save(session_id, history);
}

void CostStore::Save(const std::string& session_id, const SessionHistory& history) {
    if (storage_ == nullptr) return;  // degraded: no storage seam, no history
    nlohmann::json document = nlohmann::json::object();
    document[kParentKey] = history.parent_session_id;
    nlohmann::json turns = nlohmann::json::array();
    for (const auto& record : history.turns) turns.push_back(EncodeTurn(record));
    document[kTurnsKey] = std::move(turns);
    storage_->Set(kStorageNamespace, kSessionKeyPrefix + session_id, document.dump());
}

SessionHistory CostStore::Load(const std::string& session_id) const {
    if (storage_ == nullptr) return {};
    const auto existing = storage_->Get(kStorageNamespace, kSessionKeyPrefix + session_id);
    if (!existing) return {};
    const nlohmann::json document = ParseOrThrow(*existing, session_id);
    if (!document.is_object()) {
        throw std::runtime_error("cost-tracker: stored history for session " + session_id +
                                 " is not an object");
    }
    SessionHistory history;
    if (const auto it = document.find(kParentKey); it != document.end() && it->is_string()) {
        history.parent_session_id = it->get<std::string>();
    }
    if (const auto it = document.find(kTurnsKey); it != document.end() && it->is_array()) {
        for (const auto& entry : *it) history.turns.push_back(DecodeTurn(entry, session_id));
    }
    return history;
}

std::vector<std::string> CostStore::SessionIds() const {
    if (storage_ == nullptr) return {};
    std::vector<std::string> ids;
    for (const auto& key : storage_->Keys(kStorageNamespace, kSessionKeyPrefix)) {
        ids.push_back(key.substr(std::string_view(kSessionKeyPrefix).size()));
    }
    return ids;
}

std::optional<FetchedRates> CostStore::GetFetchedRates(const std::string& source_name) const {
    if (storage_ == nullptr) return std::nullopt;
    const auto blob = storage_->Get(kStorageNamespace, FetchedRatesKey(source_name));
    if (!blob) return std::nullopt;
    return FetchedFromJson(*blob);
}

std::vector<std::string> CostStore::PruneFetchedRates(const std::vector<std::string>& configured) {
    std::vector<std::string> removed;
    if (storage_ == nullptr) return removed;
    // The prefix WITHOUT its slash: the snapshots stored before rate_sources
    // ("rates/fetched", "rates/fetched_openrouter" -- one the full LiteLLM
    // catalog) are no source's key, and went unread and undeleted forever.
    const std::string_view prefix(kFetchedRatesKeyPrefix);
    for (const auto& key :
         storage_->Keys(kStorageNamespace, std::string(prefix.substr(0, prefix.size() - 1)))) {
        const bool current = key.rfind(prefix, 0) == 0;
        std::string name = current ? key.substr(prefix.size()) : key;
        if (current && std::find(configured.begin(), configured.end(), name) != configured.end()) {
            continue;
        }
        if (storage_->Delete(kStorageNamespace, key)) removed.push_back(std::move(name));
    }
    std::sort(removed.begin(), removed.end());
    return removed;
}

void CostStore::SetFetchedRates(const std::string& source_name, const FetchedRates& rates) {
    if (storage_ == nullptr) return;
    storage_->Set(kStorageNamespace, FetchedRatesKey(source_name), FetchedToJson(rates).dump());
}

}  // namespace minimal::cost_tracker
