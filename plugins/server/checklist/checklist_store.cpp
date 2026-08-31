#include "checklist_store.hpp"

#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>

namespace minimal::checklist {
namespace {

// The stored shape. Versioned implicitly by DecodeItems' strictness: a future
// field lands additively here and the decoder ignores unknown keys -- but a
// REMOVED or re-typed field is a decode failure, which is what makes a newer
// plugin's rows refuse an older binary instead of reading as empty.
constexpr const char* kItemsKey = "items";
constexpr const char* kIdKey = "id";
constexpr const char* kTextKey = "text";
constexpr const char* kStatusKey = "status";

nlohmann::json ParseOrThrow(std::string_view json) {
    try {
        return nlohmann::json::parse(json);
    } catch (const std::exception& error) {
        throw std::runtime_error(std::string("checklist: stored state is not valid JSON: ") +
                                 error.what());
    }
}

Item DecodeItem(const nlohmann::json& entry) {
    if (!entry.is_object()) {
        throw std::runtime_error("checklist: stored item is not an object");
    }
    Item item;
    if (!entry.contains(kIdKey) || !entry.at(kIdKey).is_number_integer()) {
        throw std::runtime_error("checklist: stored item has no integer id");
    }
    item.id = entry.at(kIdKey).get<std::int64_t>();
    if (!entry.contains(kTextKey) || !entry.at(kTextKey).is_string()) {
        throw std::runtime_error("checklist: stored item has no text");
    }
    item.text = entry.at(kTextKey).get<std::string>();
    item.status = ItemStatus::Pending;
    if (entry.contains(kStatusKey)) {
        if (!entry.at(kStatusKey).is_string()) {
            throw std::runtime_error("checklist: stored item status is not a string");
        }
        const auto status = ParseStatus(entry.at(kStatusKey).get<std::string>());
        if (!status) {
            throw std::runtime_error("checklist: stored item status '" +
                                     entry.at(kStatusKey).get<std::string>() + "' is unknown");
        }
        item.status = *status;
    }
    return item;
}

}  // namespace

std::optional<ItemStatus> ParseStatus(std::string_view name) {
    if (name == "pending") return ItemStatus::Pending;
    if (name == "in_progress") return ItemStatus::InProgress;
    if (name == "completed") return ItemStatus::Completed;
    return std::nullopt;
}

std::string_view StatusName(ItemStatus status) {
    switch (status) {
        case ItemStatus::Pending: return "pending";
        case ItemStatus::InProgress: return "in_progress";
        case ItemStatus::Completed: return "completed";
    }
    return "pending";
}

ChecklistStore::ChecklistStore(plugins::PluginStorage* storage) : storage_(storage) {}

std::vector<Item> ChecklistStore::Load(const std::string& session_id) const {
    if (storage_ == nullptr || session_id.empty()) return {};
    const auto stored = storage_->Get(kStorageNamespace, session_id);
    if (!stored) return {};
    return DecodeItems(*stored);
}

void ChecklistStore::Save(const std::string& session_id, const std::vector<Item>& items) {
    if (storage_ == nullptr) {
        throw std::runtime_error("checklist: plugin storage is not available in this host");
    }
    if (session_id.empty()) {
        throw std::runtime_error("checklist: no session to store the list under");
    }
    storage_->Set(kStorageNamespace, session_id, EncodeItems(items));
}

std::vector<std::string> ChecklistStore::SessionIds() const {
    if (storage_ == nullptr) return {};
    return storage_->Keys(kStorageNamespace);
}

std::vector<Item> DecodeItems(std::string_view json) {
    const auto document = ParseOrThrow(json);
    if (!document.is_object() || !document.contains(kItemsKey) ||
        !document.at(kItemsKey).is_array()) {
        throw std::runtime_error("checklist: stored state has no items array");
    }
    std::vector<Item> items;
    items.reserve(document.at(kItemsKey).size());
    for (const auto& entry : document.at(kItemsKey)) {
        items.push_back(DecodeItem(entry));
    }
    return items;
}

std::string EncodeItems(const std::vector<Item>& items) {
    auto entries = nlohmann::json::array();
    for (const auto& item : items) {
        entries.push_back({{kIdKey, item.id},
                           {kTextKey, item.text},
                           {kStatusKey, StatusName(item.status)}});
    }
    return nlohmann::json({{kItemsKey, std::move(entries)}}).dump();
}

std::string RenderMarkdown(const std::vector<Item>& items) {
    if (items.empty()) return "Checklist is empty.";
    std::string out;
    int done = 0;
    for (const auto& item : items) {
        if (item.status == ItemStatus::Completed) ++done;
        out += "- [";
        out += (item.status == ItemStatus::Completed) ? "x" : " ";
        out += "] ";
        if (item.status == ItemStatus::InProgress) out += "*(in progress)* ";
        out += item.text;
        out += "\n";
    }
    out += "\n" + std::to_string(done) + "/" + std::to_string(items.size()) + " done";
    return out;
}

}  // namespace minimal::checklist
