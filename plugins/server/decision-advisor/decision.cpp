#include "decision.hpp"

#include <algorithm>

namespace minimal::decision_advisor {

std::optional<ChoiceAnswer> ExtractActionAnswer(const nlohmann::json& body) {
    // The envelope is {"model", "answers", "usage"}; a non-object or a missing
    // answers map is not an answer to act on.
    if (!body.is_object()) return std::nullopt;
    auto answers = body.find("answers");
    if (answers == body.end() || !answers->is_object()) return std::nullopt;
    auto action = answers->find(kActionQuestion);
    if (action == answers->end() || !action->is_object()) return std::nullopt;

    auto choice = action->find("choice");
    auto probabilities = action->find("probabilities");
    if (choice == action->end() || !choice->is_string() || probabilities == action->end() ||
        !probabilities->is_object()) {
        return std::nullopt;
    }

    ChoiceAnswer out;
    out.choice = choice->get<std::string>();
    if (std::find(kActionOptions.begin(), kActionOptions.end(), out.choice) == kActionOptions.end()) {
        return std::nullopt;  // an option outside the fixed set: contract violation, not a decision
    }
    for (const std::string_view option : kActionOptions) {
        auto it = probabilities->find(option);
        if (it == probabilities->end() || !it->is_number()) return std::nullopt;
        const double value = it->get<double>();
        if (option == "allow") out.p_allow = value;
        else if (option == "deny") out.p_deny = value;
        else out.p_review = value;
    }
    return out;
}

Decision ResolveDecision(const ChoiceAnswer& answer, double min_probability_allow,
                         double min_probability_deny, double min_margin) {
    Decision out;
    if (answer.choice == "review") {
        out.abstain_reason = "review";
        return out;
    }

    const bool allow = answer.choice == "allow";
    const double floor = allow ? min_probability_allow : min_probability_deny;
    const double chosen = allow ? answer.p_allow : answer.p_deny;
    const double best_other = std::max(allow ? answer.p_deny : answer.p_allow, answer.p_review);

    if (chosen < floor) {
        out.abstain_reason = "probability " + std::to_string(chosen) + " < floor " +
                             std::to_string(floor);
        return out;
    }
    if (chosen - best_other < min_margin) {
        out.abstain_reason = "margin " + std::to_string(chosen - best_other) + " < " +
                             std::to_string(min_margin);
        return out;
    }
    out.action = allow ? Action::kAllow : Action::kDeny;
    return out;
}

}  // namespace minimal::decision_advisor
