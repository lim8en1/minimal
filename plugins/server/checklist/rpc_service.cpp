#include "rpc_service.hpp"

#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "minimal/common/logger.hpp"

namespace minimal::checklist {

plugins::PluginRpcHandler::Reply ChecklistRpcService::Handle(const std::string& method,
                                                             std::string_view json_payload) {
    if (!store_.available()) {
        return Reply{false, "", "checklist: plugin storage is not available in this host"};
    }
    if (method != "get" && method != "list") {
        return Reply{false, "", "checklist: unknown method \"" + method +
                                    "\" (expected \"get\" or \"list\")"};
    }

    nlohmann::json body;
    try {
        body = json_payload.empty() ? nlohmann::json::object()
                                    : nlohmann::json::parse(json_payload);
    } catch (const std::exception& error) {
        return Reply{false, "", std::string("checklist: payload is not valid JSON: ") +
                                    error.what()};
    }
    if (!body.is_object()) {
        return Reply{false, "", "checklist: payload must be a JSON object"};
    }

    if (method == "get") {
        if (!body.contains("session_id") || !body.at("session_id").is_string() ||
            body.at("session_id").get<std::string>().empty()) {
            return Reply{false, "", "checklist: get requires a non-empty string 'session_id'"};
        }
        const auto session_id = body.at("session_id").get<std::string>();
        std::vector<Item> items;
        try {
            // Load decodes the stored blob strictly; a corrupt row throws.
            items = store_.Load(session_id);
        } catch (const std::exception& error) {
            return Reply{false, "", std::string("checklist: ") + error.what()};
        }
        return Reply{true,
                     nlohmann::json({{"session_id", session_id}, {"items", ItemsJson(items)}})
                         .dump(),
                     ""};
    }

    // list
    std::vector<std::string> session_ids;
    if (const auto filter = body.find("session_ids"); filter != body.end()) {
        if (!filter->is_array()) {
            return Reply{false, "", "checklist: 'session_ids' must be an array of strings"};
        }
        for (const auto& id : *filter) {
            if (!id.is_string()) {
                return Reply{false, "", "checklist: 'session_ids' must be an array of strings"};
            }
            session_ids.push_back(id.get<std::string>());
        }
    } else {
        session_ids = store_.SessionIds();
    }

    auto entries = nlohmann::json::array();
    for (const auto& session_id : session_ids) {
        // Per-session, not around the loop: one corrupt blob must not abort
        // the whole enumeration, the same rule /cost all follows. The skip is
        // logged; the response carries no error channel for a partial listing.
        try {
            auto items = store_.Load(session_id);
            // A named session with no stored list is absent, as in the
            // unfiltered enumeration.
            if (items.empty()) continue;
            entries.push_back({{"session_id", session_id}, {"items", ItemsJson(items)}});
        } catch (const std::exception& error) {
            minimal::common::DefaultLogger().warn(
                "checklist: skipping unreadable stored list for session {}: {}", session_id,
                error.what());
        }
    }
    return Reply{true, nlohmann::json({{"checklists", std::move(entries)}}).dump(), ""};
}

}  // namespace minimal::checklist
