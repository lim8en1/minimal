#include "advisor.hpp"

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
                                 net::HttpClient http,
                                 common::Logger logger)
    : settings_(std::move(settings)),
      api_key_(std::move(api_key)),
      http_(std::move(http)),
      logger_(std::move(logger)) {}

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

    std::string key;
    try {
        if (api_key_.configured()) key = api_key_.Get().value;
    } catch (const std::exception& error) {
        logger_.warn("decision-advisor: abstained for session {} tool '{}' (api key: {})",
                     request.session_id, request.tool_name, error.what());
        return std::nullopt;
    }
    const SystemOneOutcome outcome = AskSystemOne(http_, settings_, key, StateOf(request), cancelled);
    if (!outcome.ok) {
        logger_.warn("decision-advisor: abstained for session {} tool '{}' ({}; {} attempt(s), "
                     "{}ms)",
                     request.session_id, request.tool_name, outcome.abstain_reason,
                     outcome.attempts, outcome.total.count());
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

    if (decision.action == Action::kAbstain) {
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
