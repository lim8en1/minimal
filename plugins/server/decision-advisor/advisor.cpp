#include "advisor.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <string>
#include <utility>

#include "decision.hpp"
#include "minimal/tools/permission.hpp"
#include "systemone_client.hpp"

namespace minimal::decision_advisor {
namespace {

// The answering model for the log line and deny message; a server that omits
// the envelope's "model" field still gets a readable line.
std::string ModelName(const SystemOneOutcome& outcome) {
    return outcome.answered_model.empty() ? "unknown" : outcome.answered_model;
}

}  // namespace

DecisionAdvisor::DecisionAdvisor(Settings settings, common::SecretHandle api_key,
                                 net::HttpClient http, std::filesystem::path decision_log,
                                 common::Logger logger)
    : settings_(std::move(settings)),
      api_key_(std::move(api_key)),
      http_(std::move(http)),
      decision_log_(std::move(decision_log)),
      logger_(std::move(logger)) {}

void DecisionAdvisor::LogDecision(nlohmann::json record) {
    if (decision_log_.empty()) return;
    record["ts_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
    // One line per call; replace invalid UTF-8 in tool input rather than throw.
    const std::string line =
        record.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
    std::lock_guard<std::mutex> lock(decision_log_mu_);
    std::ofstream out(decision_log_, std::ios::app | std::ios::binary);
    out << line;
    out.flush();
    if (!out && !decision_log_warned_.exchange(true)) {
        logger_.warn("decision-advisor: cannot append to decision log {}; decisions are not "
                     "being logged",
                     decision_log_.string());
    }
}

nlohmann::json DecisionAdvisor::StateOf(const tools::PermissionRequest& request) const {
    // The decision, not the world: what the judge needs to rule on this one
    // call. tool_input is JSON already; nothing else about the harness (the
    // mode that produced the Ask, grants held, history) is sent. Nor is the
    // session id: it means nothing to the judge, identifies the session to an
    // external service, and no backend keys a cache on it -- llama-server's
    // cache is a token prefix, which a per-session id at the head of the
    // state would only break.
    return nlohmann::json{{"tool_name", request.tool_name},
                          {"summary", request.summary},
                          {"tool_input", request.tool_input}};
}

std::optional<tools::PermissionResponse> DecisionAdvisor::Advise(
    const tools::PermissionRequest& request, const tools::AdvisorContext& context) {
    // The advisor contract: poll and return. A cancelled call must never spend
    // its budget -- and never reach the network. An ABSENT predicate (a
    // default-constructed AdvisorContext, as the checker-level tests pass)
    // is "not cancelled", not a crash.
    std::function<bool()> cancelled = context.is_cancelled ? context.is_cancelled
                                                           : [] { return false; };
    if (cancelled()) return std::nullopt;

    // The decision log's record: what was asked, under which thresholds.
    // Every exit below completes and writes it once.
    nlohmann::json record;
    if (!decision_log_.empty()) {
        record = {{"session_id", request.session_id},
                  {"tool_name", request.tool_name},
                  {"summary", request.summary},
                  {"tool_input", request.tool_input},
                  {"backend", settings_.backend},
                  {"model", settings_.model},
                  {"thresholds",
                   {{"min_probability_allow", settings_.min_probability_allow},
                    {"min_probability_deny", settings_.min_probability_deny},
                    {"min_margin", settings_.min_margin}}}};
    }
    const auto log_abstain = [&](const std::string& reason) {
        if (decision_log_.empty()) return;
        record["outcome"] = "abstained";
        record["reason"] = reason;
        LogDecision(std::move(record));
    };

    std::string key;
    try {
        if (api_key_.configured()) key = api_key_.Get().value;
    } catch (const std::exception& error) {
        logger_.warn("decision-advisor: abstained for session {} tool '{}' (api key: {})",
                     request.session_id, request.tool_name, error.what());
        log_abstain(std::string("api key: ") + error.what());
        return std::nullopt;
    }
    const SystemOneOutcome outcome = AskSystemOne(http_, settings_, key, StateOf(request), cancelled);
    if (!decision_log_.empty()) {
        record["answered_model"] = outcome.answered_model;
        record["attempts"] = outcome.attempts;
        record["total_ms"] = outcome.total.count();
        // Verbatim: the action answer's probabilities and confidence, and
        // every extra_questions answer beside it.
        if (outcome.ok) record["answers"] = outcome.answer;
    }
    if (!outcome.ok) {
        logger_.warn("decision-advisor: abstained for session {} tool '{}' ({}; {} attempt(s), "
                     "{}ms)",
                     request.session_id, request.tool_name, outcome.abstain_reason,
                     outcome.attempts, outcome.total.count());
        log_abstain(outcome.abstain_reason);
        return std::nullopt;
    }

    // AskSystemOne's own ExtractActionAnswer call (the one that validated the
    // envelope enough to set outcome.ok) already produced this -- re-wrapping
    // outcome.answer into a fake envelope and parsing it again here would be
    // redundant work on every successful call, and a second place the two
    // call sites could drift if ExtractActionAnswer's contract changed.
    const auto& answer = outcome.choice;
    if (answer == std::nullopt) {
        // AskSystemOne already refused to return an envelope without a choice
        // answer; reaching here would mean the parse contract drifted.
        logger_.warn("decision-advisor: abstained for session {} tool '{}' (answer shape "
                     "changed)",
                     request.session_id, request.tool_name);
        log_abstain("answer shape changed");
        return std::nullopt;
    }

    const Decision decision =
        ResolveDecision(*answer, settings_.min_probability_allow, settings_.min_probability_deny,
                        settings_.min_margin);
    const double chosen = answer->choice == "allow"   ? answer->p_allow
                          : answer->choice == "deny" ? answer->p_deny
                                                     : answer->p_review;
    const double best_other = answer->choice == "allow"   ? std::max(answer->p_deny, answer->p_review)
                              : answer->choice == "deny" ? std::max(answer->p_allow, answer->p_review)
                                                         : std::max(answer->p_allow, answer->p_deny);
    if (!decision_log_.empty()) {
        record["choice"] = answer->choice;
        record["probabilities"] = {
            {"allow", answer->p_allow}, {"deny", answer->p_deny}, {"review", answer->p_review}};
        if (decision.action != Action::kAbstain) {
            record["outcome"] = decision.action == Action::kAllow ? "allowed" : "denied";
            LogDecision(std::move(record));
        }
    }

    if (decision.action == Action::kAbstain) {
        log_abstain(decision.abstain_reason);
        logger_.info(
            "decision-advisor: session {} tool '{}' -> abstained ({}) [model={} choice={} "
            "p1={:.3f} p2={:.3f} attempts={} total_ms={}]",
            request.session_id, request.tool_name, decision.abstain_reason, ModelName(outcome),
            answer->choice, chosen, best_other, outcome.attempts, outcome.total.count());
        return std::nullopt;
    }

    if (decision.action == Action::kDeny) {
        logger_.info(
            "decision-advisor: session {} tool '{}' -> denied [model={} choice={} p1={:.3f} "
            "p2={:.3f} attempts={} total_ms={}]",
            request.session_id, request.tool_name, ModelName(outcome), answer->choice, chosen,
            best_other, outcome.attempts, outcome.total.count());
        tools::PermissionResponse response;
        response.decision = tools::PermissionDecision::Deny;
        response.deny_message = "auto-denied by decision-advisor (model " + ModelName(outcome) +
                                "): p(" + answer->choice + ")=" + std::to_string(chosen) + "; " +
                                settings_.deny_message;
        return response;
    }

    logger_.info(
        "decision-advisor: session {} tool '{}' -> allowed [model={} choice={} p1={:.3f} "
        "p2={:.3f} attempts={} total_ms={}]",
        request.session_id, request.tool_name, ModelName(outcome), answer->choice, chosen,
        best_other, outcome.attempts, outcome.total.count());
    tools::PermissionResponse response;
    // This invocation only: no granted_keys. The advised checker discards an
    // advisor's grants anyway -- a blanket grant would let one confident
    // allow skip every later judgement of that binary.
    response.decision = tools::PermissionDecision::Allow;
    return response;
}

}  // namespace minimal::decision_advisor
