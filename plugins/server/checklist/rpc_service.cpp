#include "rpc_service.hpp"

#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "minimal/common/logger.hpp"

namespace minimal::checklist {
namespace {

nlohmann::json ItemsJson(const std::vector<Item>& items) {
    auto entries = nlohmann::json::array();
    for (const auto& item : items) {
        entries.push_back({{"id", item.id},
                           {"text", item.text},
                           {"status", StatusName(item.status)}});
    }
    return entries;
}

}  // namespace

ChecklistRpcService::ChecklistRpcService(std::shared_ptr<ChecklistStore> store)
    : store_(std::move(store)) {}

plugins::PluginRpcHandler::Reply ChecklistRpcService::Handle(const std::string& method,
                                                             std::string_view json_payload) {
    if (store_ == nullptr || !store_->available()) {
        return Reply{false, "", "checklist: plugin storage is not available in this host"};
    }

    if (method == "get") {
        nlohmann::json body;
        try {
            body = json_payload.empty() ? nlohmann::json::object()
                                        : nlohmann::json::parse(json_payload);
        } catch (const std::exception& error) {
            return Reply{false, "", std::string("checklist: payload is not valid JSON: ") +
                                     error.what()};
        }
        if (!body.is_object() || !body.contains("session_id") || !body.at("session_id").is_string() ||
            body.at("session_id").get<std::string>().empty()) {
            return Reply{false, "", "checklist: get requires a non-empty string 'session_id'"};
        }
        const auto session_id = body.at("session_id").get<std::string>();
        std::vector<Item> items;
        try {
            // Load decodes the stored blob strictly; a corrupt row throws.
            items = store_->Load(session_id);
        } catch (const std::exception& error) {
            return Reply{false, "", std::string("checklist: ") + error.what()};
        }
        return Reply{true,
                     nlohmann::json({{"session_id", session_id}, {"items", ItemsJson(items)}})
                         .dump(),
                     ""};
    }

    if (method == "list") {
        std::vector<std::pair<std::string, std::vector<Item>>> checklists;
        for (const auto& session_id : store_->SessionIds()) {
            // Per-session, not around the loop: one corrupt blob must not
            // abort the whole enumeration, the same rule /cost all follows.
            // The skip is logged; the response shape stays {"checklists":
            // [...]} and carries no error channel for a partial listing.
            try {
                checklists.emplace_back(session_id, store_->Load(session_id));
            } catch (const std::exception& error) {
                minimal::common::DefaultLogger().warn(
                    "checklist: skipping unreadable stored list for session {}: {}",
                    session_id, error.what());
            }
        }
        auto entries = nlohmann::json::array();
        for (auto& [session_id, items] : checklists) {
            entries.push_back({{"session_id", session_id}, {"items", ItemsJson(items)}});
        }
        return Reply{true, nlohmann::json({{"checklists", std::move(entries)}}).dump(), ""};
    }

    return Reply{false, "", "checklist: unknown method \"" + method +
                                "\" (expected \"get\" or \"list\")"};
}

}  // namespace minimal::checklist
