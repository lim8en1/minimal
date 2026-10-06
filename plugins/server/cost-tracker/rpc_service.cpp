#include "rpc_service.hpp"

#include <algorithm>
#include <iterator>
#include <vector>

#include "minimal/common/logger.hpp"
#include "rates_fetch.hpp"

namespace minimal::cost_tracker {
namespace {

nlohmann::json ParsePayload(std::string_view json_payload, bool& ok, std::string& error) {
    nlohmann::json payload = nlohmann::json::object();
    if (!json_payload.empty()) {
        try {
            payload = nlohmann::json::parse(json_payload);
        } catch (const std::exception& err) {
            ok = false;
            error = std::string("payload is not valid JSON: ") + err.what();
            return payload;
        }
    }
    if (!payload.is_object()) {
        ok = false;
        error = "payload must be a JSON object";
    }
    return payload;
}

}  // namespace

nlohmann::json TotalsAsJson(const SessionTotals& totals) {
    nlohmann::json models = nlohmann::json::array();
    for (const auto& model : totals.models) {
        models.push_back({
            {"model", model.model},
            {"turns", model.turns},
            {"input_tokens", model.input_tokens},
            {"output_tokens", model.output_tokens},
            {"cache_read_tokens", model.cache_read_tokens},
            {"cache_write_tokens", model.cache_write_tokens},
            {"unpriced_turns", model.unpriced_turns},
            {"cost", model.cost},
            {"cost_known", model.cost_known},
        });
    }
    // Structured costs carry no formatting: the client decides how to render
    // dollars (locale, precision). `currency` names the unit; the "+"-marker
    // of the text path becomes the cost_known flags here.
    return nlohmann::json{
        {"session_id", totals.session_id},
        {"parent_session_id", totals.parent_session_id},
        {"turns", totals.total_turns},
        {"unpriced_turns", totals.unpriced_turns},
        {"total_cost", totals.total_cost},
        {"total_cost_known", totals.total_cost_known},
        {"currency", "USD"},
        {"models", std::move(models)},
    };
}

std::string TotalsToJson(const SessionTotals& totals) { return TotalsAsJson(totals).dump(); }

std::optional<nlohmann::json> CostRpcService::SubtreeTotals(const std::string& session_id) {
    // The requested session's WHOLE SUBAGENT SUBTREE, not just its own blob:
    // a subagent's turns are recorded under the child session's id (its
    // parent rides along in the blob), so a status line that asked only for
    // the focused session would never see what its subagents spend -- the
    // operator's bill is the family's. The index names the subtree; only its
    // members' blobs are loaded.
    const std::vector<std::string> subtree = index_.Subtree(session_id);

    // Stored turns plus every member's in-flight provisional rounds (a fresh
    // session mid-first-turn has a live total and NO stored blob yet -- the
    // live rows must not be dropped for that).
    std::vector<TurnRecord> turns;
    std::string parent_session_id;
    bool in_flight = false;
    for (const auto& id : subtree) {
        try {
            SessionHistory history = store_.Load(id);
            if (id == session_id) parent_session_id = history.parent_session_id;
            turns.insert(turns.end(), std::make_move_iterator(history.turns.begin()),
                         std::make_move_iterator(history.turns.end()));
        } catch (const std::exception& error) {
            // One corrupt blob loses one session's share of the subtotal, not
            // the whole answer -- the same skip rule the "list" path follows.
            minimal::common::DefaultLogger().warn(
                "cost-tracker: skipping unreadable cost history for session {}: {}", id,
                error.what());
        }
        auto live = in_flight_.Records(id);
        if (!live.empty()) in_flight = true;
        turns.insert(turns.end(), std::make_move_iterator(live.begin()),
                     std::make_move_iterator(live.end()));
    }
    if (turns.empty()) return std::nullopt;

    const auto totals = Aggregate(session_id, parent_session_id, turns,
                                  [this](const std::string& model) { return rates_.Resolve(model); });
    nlohmann::json body = TotalsAsJson(totals);
    body["includes_subagents"] = subtree.size() > 1;
    body["in_flight"] = in_flight;
    return body;
}

plugins::PluginRpcHandler::Reply CostRpcService::Handle(const std::string& method,
                                                        std::string_view json_payload) {
    Reply reply;
    bool payload_ok = true;
    std::string payload_error;
    const nlohmann::json payload = ParsePayload(json_payload, payload_ok, payload_error);
    if (!payload_ok) {
        reply.ok = false;
        reply.error = payload_error;
        return reply;
    }

    if (method == "totals") {
        const auto it = payload.find("session_id");
        if (it == payload.end() || !it->is_string() || it->get<std::string>().empty()) {
            reply.ok = false;
            reply.error = "totals requires a non-empty \"session_id\" string";
            return reply;
        }
        const std::string session_id = it->get<std::string>();
        try {
            auto body = SubtreeTotals(session_id);
            if (!body.has_value()) {
                reply.ok = false;
                reply.error = "no recorded cost for session " + session_id;
                return reply;
            }
            reply.payload = body->dump();
        } catch (const std::exception& error) {
            reply.ok = false;
            reply.error = error.what();
        }
        return reply;
    }

    if (method == "list") {
        // The try lives INSIDE the loop: one corrupt blob used to abort the
        // whole listing, while the /cost all text path skips per-session --
        // the RPC surface now follows the same rule. Skips are logged; the
        // response shape carries no error channel for a partial listing.
        nlohmann::json sessions = nlohmann::json::array();
        for (const auto& session_id : store_.SessionIds()) {
            try {
                const auto history = store_.Load(session_id);
                sessions.push_back(TotalsAsJson(Aggregate(
                    session_id, history.parent_session_id, history.turns,
                    [this](const std::string& model) { return rates_.Resolve(model); })));
            } catch (const std::exception& error) {
                minimal::common::DefaultLogger().warn(
                    "cost-tracker: skipping unreadable cost history for session {}: {}",
                    session_id, error.what());
            }
        }
        reply.payload = nlohmann::json{{"sessions", std::move(sessions)}}.dump();
        return reply;
    }

    if (method == "refresh_rates") {
        const auto outcome = FetchAndUpdateRates(settings_, store_, rates_);
        if (!outcome.ok) {
            reply.ok = false;
            reply.error = outcome.error;
            return reply;
        }
        nlohmann::json sources = nlohmann::json::array();
        for (const auto& source : outcome.sources) {
            nlohmann::json entry{{"name", source.name}, {"updated", source.updated}};
            if (!source.error.empty()) entry["error"] = source.error;
            sources.push_back(std::move(entry));
        }
        reply.payload = nlohmann::json{{"updated", outcome.updated},
                                       {"sources", std::move(sources)},
                                       {"fetched_at", outcome.fetched_at},
                                       {"warning", outcome.warning}}
                            .dump();
        return reply;
    }

    reply.ok = false;
    reply.error = "unknown method '" + method + "'";
    return reply;
}

}  // namespace minimal::cost_tracker
