#include "decision.hpp"

namespace minimal::decision_advisor {

namespace {

const char* kActionKey = "action";

}  // namespace

std::optional<ChoiceAnswer> ExtractActionAnswer(const nlohmann::json& body) {
    // The envelope is {"model", "answers", "usage"}; a non-object or a missing
    // answers map is not an answer to act on.
    if (!body.is_object()) return std::nullopt;
    auto answers = body.find("answers");
    if (answers == body.end() || !answers->is_object()) return std::nullopt;
    auto action = answers->find(kActionKey);
    if (action == answers->end() || !action->is_object()) return std::nullopt;

    auto choice = action->find("choice");
    auto probabilities = action->find("probabilities");
    if (choice == action->end() || !choice->is_string() || probabilities == action->end() ||
        !probabilities->is_object()) {
        return std::nullopt;
    }

    ChoiceAnswer out;
    out.choice = choice->get<std::string>();
    if (out.choice != "allow" && out.choice != "deny" && out.choice != "review") {
        return std::nullopt;  // an option outside the fixed set: contract violation, not a decision
    }
    for (const auto* option : {"allow", "deny", "review"}) {
        auto it = probabilities->find(option);
        const double value = it != probabilities->end() && it->is_number() ? it->get<double>() : 0.0;
        if (std::string_view(option) == "allow") out.p_allow = value;
        else if (std::string_view(option) == "deny") out.p_deny = value;
        else out.p_review = value;
    }
    return out;
}

Decision ResolveDecision(const ChoiceAnswer& answer, double min_probability_allow,
                         double min_probability_deny, double min_margin) {
    Decision out;

    const double floor = answer.choice == "allow"   ? min_probability_allow
                         : answer.choice == "deny" ? min_probability_deny
                                                   : 0.0;
    const double chosen = answer.choice == "allow"   ? answer.p_allow
                          : answer.choice == "deny" ? answer.p_deny
                                                    : answer.p_review;
    const double best_other = answer.choice == "allow"   ? std::max(answer.p_deny, answer.p_review)
                              : answer.choice == "deny" ? std::max(answer.p_allow, answer.p_review)
                                                        : std::max(answer.p_allow, answer.p_deny);

    if (answer.choice == "review") {
        out.abstain_reason = "review";
        return out;
    }
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
    out.action = answer.choice == "allow" ? Action::kAllow : Action::kDeny;
    return out;
}

}  // namespace minimal::decision_advisor
