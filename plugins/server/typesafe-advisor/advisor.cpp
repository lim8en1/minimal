#include "advisor.hpp"

#include <string>
#include <utility>

#include "decision.hpp"
#include "minimal/tools/permission.hpp"
#include "typesafe_client.hpp"

namespace minimal::typesafe_advisor {

TypesafeAdvisor::TypesafeAdvisor(Settings settings, std::string api_key, net::HttpClient http,
                                 common::Logger logger)
    : settings_(std::move(settings)),
      api_key_(std::move(api_key)),
      http_(std::move(http)),
      logger_(std::move(logger)) {}

nlohmann::json TypesafeAdvisor::StateOf(const tools::PermissionRequest& request) const {
    // The decision, not the world: what the judge needs to rule on this one
    // call. tool_input is JSON already; nothing else about the harness (the
    // mode that produced the Ask, grants held, history) is sent.
    return nlohmann::json{{"session_id", request.session_id},
                          {"tool_name", request.tool_name},
                          {"summary", request.summary},
                          {"tool_input", request.tool_input}};
}

std::optional<tools::PermissionResponse> TypesafeAdvisor::Advise(
    const tools::PermissionRequest& request, const tools::AdvisorContext& context) {
    // The advisor contract: poll and return. A cancelled call must never spend
    // its budget -- and never reach the network. An ABSENT predicate (a
    // default-constructed AdvisorContext, as the checker-level tests pass)
    // is "not cancelled", not a crash.
    std::function<bool()> cancelled = context.is_cancelled ? context.is_cancelled
                                                           : [] { return false; };
    if (cancelled()) return std::nullopt;

    const JevOutcome outcome = AskJev(http_, settings_, api_key_, StateOf(request), cancelled);
    if (!outcome.ok) {
        logger_.warn("typesafe-advisor: abstained for session {} tool '{}' ({}; {} attempt(s), "
                     "{}ms)",
                     request.session_id, request.tool_name, outcome.abstain_reason,
                     outcome.attempts, outcome.total.count());
        return std::nullopt;
    }

    const auto answer = ExtractActionAnswer(nlohmann::json{{"answers", outcome.answer}});
    if (answer == std::nullopt) {
        // AskJev already refused to return an envelope without a choice
        // answer; reaching here would mean the parse contract drifted.
        logger_.warn("typesafe-advisor: abstained for session {} tool '{}' (answer shape "
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
            "typesafe-advisor: session {} tool '{}' -> abstained ({}) [model={} choice={} "
            "p1={:.3f} p2={:.3f} attempts={} total_ms={}]",
            request.session_id, request.tool_name, decision.abstain_reason,
            outcome.answered_model.empty() ? "unknown" : outcome.answered_model, answer->choice,
            chosen, best_other, outcome.attempts, outcome.total.count());
        return std::nullopt;
    }

    if (decision.action == Action::kDeny) {
        logger_.info(
            "typesafe-advisor: session {} tool '{}' -> denied [model={} choice={} p1={:.3f} "
            "p2={:.3f} attempts={} total_ms={}]",
            request.session_id, request.tool_name,
            outcome.answered_model.empty() ? "unknown" : outcome.answered_model, answer->choice,
            chosen, best_other, outcome.attempts, outcome.total.count());
        tools::PermissionResponse response;
        response.decision = tools::PermissionDecision::Deny;
        response.deny_message =
            "auto-denied by typesafe-advisor (model " +
            (outcome.answered_model.empty() ? std::string("unknown") : outcome.answered_model) +
            "): p(" + answer->choice + ")=" + std::to_string(chosen) +
            "; the call looks destructive or out of scope -- ask the user before retrying";
        return response;
    }

    logger_.info(
        "typesafe-advisor: session {} tool '{}' -> allowed [model={} choice={} p1={:.3f} "
        "p2={:.3f} attempts={} total_ms={}]",
        request.session_id, request.tool_name,
        outcome.answered_model.empty() ? "unknown" : outcome.answered_model, answer->choice,
        chosen, best_other, outcome.attempts, outcome.total.count());
    tools::PermissionResponse response;
    response.decision = tools::PermissionDecision::Allow;
    // Every key the disclosure offered, granted only through the double filter
    // downstream: the offer is what the operator WOULD have been shown, and
    // ToolExecutor re-checks each key against it before remembering any.
    for (const auto& offer : request.disclosure.grants) {
        response.granted_keys.push_back(offer.key);
    }
    return response;
}

}  // namespace minimal::typesafe_advisor
